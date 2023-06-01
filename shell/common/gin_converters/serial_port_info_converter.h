// Copyright (c) 2022 Microsoft, Inc.
// Use of this source code is governed by the MIT license that can be
// found in the LICENSE file.

#ifndef ELECTRON_SHELL_COMMON_GIN_CONVERTERS_SERIAL_PORT_INFO_CONVERTER_H_
#define ELECTRON_SHELL_COMMON_GIN_CONVERTERS_SERIAL_PORT_INFO_CONVERTER_H_

#include "gin/converter.h"
#include "services/device/public/mojom/serial.mojom.h"
#include "shell/browser/serial/serial_chooser_context.h"
#include "shell/common/gin_converters/value_converter.h"

namespace gin {

template <>
struct Converter<device::mojom::SerialPortInfoPtr> {
  static v8::Local<v8::Value> ToV8(
      v8::Isolate* isolate,
      const device::mojom::SerialPortInfoPtr& port) {
    base::Value value =
        electron::SerialChooserContext::PortInfoToValue(*port, false);
    return gin::ConvertToV8(isolate, value);
  }
};

}  // namespace gin

#endif  // ELECTRON_SHELL_COMMON_GIN_CONVERTERS_SERIAL_PORT_INFO_CONVERTER_H_
