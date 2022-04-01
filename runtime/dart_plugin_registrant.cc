// Copyright 2013 The Flutter Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "flutter/runtime/dart_plugin_registrant.h"

#include <string>

#include "flutter/fml/logging.h"
#include "flutter/fml/trace_event.h"
#include "third_party/tonic/converter/dart_converter.h"
#include "third_party/tonic/logging/dart_invoke.h"

namespace flutter {

const char* dart_plugin_registrant_library_override = nullptr;

bool InvokeDartPluginRegistrantIfAvailable(Dart_Handle library_handle) {
  TRACE_EVENT0("flutter", "InvokeDartPluginRegistrantIfAvailable");

  // The Dart plugin registrant is a static method with signature `void
  // register()` within the class `_PluginRegistrant` generated by the Flutter
  // tool.
  //
  // This method binds a plugin implementation to their platform
  // interface based on the configuration of the app's pubpec.yaml, and the
  // plugin's pubspec.yaml.
  //
  // Since this method may or may not be defined, check if the class is defined
  // in the default library before calling the method.
  Dart_Handle plugin_registrant =
      ::Dart_GetClass(library_handle, tonic::ToDart("_PluginRegistrant"));

  if (Dart_IsError(plugin_registrant)) {
    return false;
  }
  tonic::LogIfError(tonic::DartInvokeField(plugin_registrant, "register", {}));
  return true;
}

bool FindAndInvokeDartPluginRegistrant() {
  std::string library_name =
      dart_plugin_registrant_library_override == nullptr
          ? "package:flutter/src/dart_plugin_registrant.dart"
          : dart_plugin_registrant_library_override;
  Dart_Handle library = Dart_LookupLibrary(tonic::ToDart(library_name));
  if (Dart_IsError(library)) {
    return false;
  }
  Dart_Handle registrant_file_uri =
      Dart_GetField(library, tonic::ToDart("dartPluginRegistrantLibrary"));
  if (Dart_IsError(registrant_file_uri)) {
    // TODO(gaaclarke): Find a way to remove this branch so the field is
    // required. I couldn't get it working with unit tests.
    return InvokeDartPluginRegistrantIfAvailable(library);
  }

  std::string registrant_file_uri_string =
      tonic::DartConverter<std::string>::FromDart(registrant_file_uri);
  if (registrant_file_uri_string.empty()) {
    return false;
  }

  Dart_Handle registrant_library = Dart_LookupLibrary(registrant_file_uri);
  return InvokeDartPluginRegistrantIfAvailable(registrant_library);
}
}  // namespace flutter
