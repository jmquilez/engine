// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/shell/common/vsync_waiter.h"
#include <sys/time.h>
#include <tonic/typed_data/dart_byte_data.h>

#include "flow/frame_timings.h"
#include "flutter/fml/task_runner.h"
#include "flutter/fml/trace_event.h"
#include "fml/logging.h"
#include "fml/message_loop_task_queues.h"
#include "fml/task_queue_id.h"
#include "fml/time/time_point.h"

namespace flutter {

static constexpr const char* kVsyncFlowName = "VsyncFlow";

static constexpr const char* kVsyncTraceName = "VsyncProcessCallback";

fml::TimePoint LastVsyncInfo::GetVsyncStartTime() const {
  std::scoped_lock state_lock(mutex_);
  return vsync_start_;
}

fml::TimePoint LastVsyncInfo::GetVsyncTargetTime() const {
  std::scoped_lock state_lock(mutex_);
  return vsync_target_;
}

int64_t LastVsyncInfo::GetVsyncTargetDatetime() const {
  std::scoped_lock state_lock(mutex_);
  return vsync_target_datetime_;
}

// ref: Dart DateTime.now() implementation
// https://github.com/fzyzcjy/yplusplus/issues/5834#issuecomment-1257329034
int64_t DartCompatibleGetCurrentTimeMicros() {
  // gettimeofday has microsecond resolution.
  struct timeval tv;
  if (gettimeofday(&tv, NULL) < 0) {
    return 0;
  }
  return (static_cast<int64_t>(tv.tv_sec) * 1000000) + tv.tv_usec;
}

void LastVsyncInfo::RecordVsync(fml::TimePoint vsync_start,
                                fml::TimePoint vsync_target) {
  //  FML_DLOG(INFO) << "hi LastVsyncInfo::RecordVsync"
  //                 << " this_thread_id=" << pthread_self() << " vsync_start="
  //                 << (vsync_start - fml::TimePoint()).ToMicroseconds()
  //                 << " vsync_target="
  //                 << (vsync_target - fml::TimePoint()).ToMicroseconds();

  int64_t curr_datetime = DartCompatibleGetCurrentTimeMicros();
  fml::TimePoint curr_time = fml::TimePoint::Now();
  int64_t vsync_target_datetime =
      curr_datetime + (vsync_target - curr_time).ToMicroseconds();

  std::scoped_lock state_lock(mutex_);
  vsync_start_ = vsync_start;
  vsync_target_ = vsync_target;
  vsync_target_datetime_ = vsync_target_datetime;
}

LastVsyncInfo& LastVsyncInfo::Instance() {
  static LastVsyncInfo instance;
  return instance;
}

Dart_Handle LastVsyncInfo::ReadToDart() {
  LastVsyncInfo& instance = Instance();
  auto vsync_target_time = instance.GetVsyncTargetTime();
  auto vsync_target_datetime = instance.GetVsyncTargetDatetime();

  // ref OnAnimatorBeginFrame -> ... -> begin_frame_, uses GetVsyncTargetTime
  // ref PlatformConfiguration::BeginFrame
  int64_t vsync_target_time_us =
      (vsync_target_time - fml::TimePoint()).ToMicroseconds();

  std::vector<int64_t> data{vsync_target_time_us, vsync_target_datetime};
  return tonic::DartConverter<std::vector<int64_t>>::ToDart(data);
}

VsyncWaiter::VsyncWaiter(TaskRunners task_runners)
    : task_runners_(std::move(task_runners)) {}

VsyncWaiter::~VsyncWaiter() = default;

// Public method invoked by the animator.
void VsyncWaiter::AsyncWaitForVsync(const Callback& callback) {
  if (!callback) {
    return;
  }

  TRACE_EVENT0("flutter", "AsyncWaitForVsync");

  {
    std::scoped_lock lock(callback_mutex_);
    if (callback_) {
      // The animator may request a frame more than once within a frame
      // interval. Multiple calls to request frame must result in a single
      // callback per frame interval.
      TRACE_EVENT_INSTANT0("flutter", "MultipleCallsToVsyncInFrameInterval");
      return;
    }
    callback_ = std::move(callback);
    if (!secondary_callbacks_.empty()) {
      // Return directly as `AwaitVSync` is already called by
      // `ScheduleSecondaryCallback`.
      return;
    }
  }
  AwaitVSync();
}

void VsyncWaiter::ScheduleSecondaryCallback(uintptr_t id,
                                            const fml::closure& callback,
                                            bool sanity_check_thread) {
  // NOTE HACK #5831
  FML_DCHECK(!sanity_check_thread ||
             task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread());

  if (!callback) {
    return;
  }

  TRACE_EVENT0("flutter", "ScheduleSecondaryCallback");

  {
    std::scoped_lock lock(callback_mutex_);
    auto [_, inserted] = secondary_callbacks_.emplace(id, std::move(callback));
    if (!inserted) {
      // Multiple schedules must result in a single callback per frame interval.
      TRACE_EVENT_INSTANT0("flutter",
                           "MultipleCallsToSecondaryVsyncInFrameInterval");
      return;
    }
    if (callback_) {
      // Return directly as `AwaitVSync` is already called by
      // `AsyncWaitForVsync`.
      return;
    }
  }
  AwaitVSyncForSecondaryCallback();
}

void VsyncWaiter::FireCallback(fml::TimePoint frame_start_time,
                               fml::TimePoint frame_target_time,
                               bool pause_secondary_tasks) {
  //  FML_DLOG(INFO)
  //      << "hi VsyncWaiter::FireCallback start"
  //      << " this_thread_id=" << pthread_self() << " is-on-platform-thread="
  //      << task_runners_.GetPlatformTaskRunner()->RunsTasksOnCurrentThread()
  //      << " is-on-ui-thread="
  //      << task_runners_.GetUITaskRunner()->RunsTasksOnCurrentThread()
  //      << " is-on-io-thread="
  //      << task_runners_.GetIOTaskRunner()->RunsTasksOnCurrentThread()
  //      << " is-on-raster-thread="
  //      << task_runners_.GetRasterTaskRunner()->RunsTasksOnCurrentThread();
  FML_DCHECK(fml::TimePoint::Now() >= frame_start_time);

  LastVsyncInfo::Instance().RecordVsync(frame_start_time, frame_target_time);

  Callback callback;
  std::vector<fml::closure> secondary_callbacks;

  {
    std::scoped_lock lock(callback_mutex_);
    callback = std::move(callback_);
    for (auto& pair : secondary_callbacks_) {
      secondary_callbacks.push_back(std::move(pair.second));
    }
    secondary_callbacks_.clear();
  }

  // hack: schedule immediately to ensure [LastVsyncInfo] is updated every 16ms
  // in real implementation, will instead have real start/pause mechanism
  // instead of such blindly refresh
  // #5831
  // NOTE HACK about threads:
  // * With current hack, FireCallback is in platform thread
  // * Current AwaitVsync (android + not-ndk) can be called in PlatformThread
  // but need hack for other platforms as well
  //  FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback extra call AwaitVsync to "
  //                    "ensure every frame we see info";
  ScheduleSecondaryCallback(
      reinterpret_cast<uintptr_t>(&LastVsyncInfo::Instance()), [] {},
      // NOTE do NOT sanity check thread, since closure is empty and we only
      // want to trigger scheduling
      false);

  if (!callback && secondary_callbacks.empty()) {
    // This means that the vsync waiter implementation fired a callback for a
    // request we did not make. This is a paranoid check but we still want to
    // make sure we catch misbehaving vsync implementations.
    TRACE_EVENT_INSTANT0("flutter", "MismatchedFrameCallback");
    return;
  }

  if (callback) {
    auto flow_identifier = fml::tracing::TraceNonce();
    if (pause_secondary_tasks) {
      PauseDartMicroTasks();
    }

    // The base trace ensures that flows have a root to begin from if one does
    // not exist. The trace viewer will ignore traces that have no base event
    // trace. While all our message loops insert a base trace trace
    // (MessageLoop::RunExpiredTasks), embedders may not.
    TRACE_EVENT0("flutter", "VsyncFireCallback");

    TRACE_FLOW_BEGIN("flutter", kVsyncFlowName, flow_identifier);

    fml::TaskQueueId ui_task_queue_id =
        task_runners_.GetUITaskRunner()->GetTaskQueueId();

    task_runners_.GetUITaskRunner()->PostTask(
        [ui_task_queue_id, callback, flow_identifier, frame_start_time,
         frame_target_time, pause_secondary_tasks]() {
          //          FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback inside
          //          UITaskRunner "
          //                            "PostTask callback start";
          FML_TRACE_EVENT("flutter", kVsyncTraceName, "StartTime",
                          frame_start_time, "TargetTime", frame_target_time);
          std::unique_ptr<FrameTimingsRecorder> frame_timings_recorder =
              std::make_unique<FrameTimingsRecorder>();
          frame_timings_recorder->RecordVsync(frame_start_time,
                                              frame_target_time);
          callback(std::move(frame_timings_recorder));
          TRACE_FLOW_END("flutter", kVsyncFlowName, flow_identifier);
          if (pause_secondary_tasks) {
            ResumeDartMicroTasks(ui_task_queue_id);
          }
          //          FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback inside
          //          UITaskRunner "
          //                            "PostTask callback end";
        });
  }

  for (auto& secondary_callback : secondary_callbacks) {
    task_runners_.GetUITaskRunner()->PostTask(std::move(secondary_callback));
  }
  //  FML_DLOG(INFO) << "hi VsyncWaiter::FireCallback end";
}

void VsyncWaiter::PauseDartMicroTasks() {
  auto ui_task_queue_id = task_runners_.GetUITaskRunner()->GetTaskQueueId();
  auto task_queues = fml::MessageLoopTaskQueues::GetInstance();
  task_queues->PauseSecondarySource(ui_task_queue_id);
}

void VsyncWaiter::ResumeDartMicroTasks(fml::TaskQueueId ui_task_queue_id) {
  auto task_queues = fml::MessageLoopTaskQueues::GetInstance();
  task_queues->ResumeSecondarySource(ui_task_queue_id);
}

}  // namespace flutter
