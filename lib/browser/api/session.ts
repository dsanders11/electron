import { fetchWithSession } from '@electron/internal/browser/api/net-fetch';
import * as deprecate from '@electron/internal/common/deprecate';
const { fromPartition, fromPath, Session } = process._linkedBinding('electron_browser_session');

Session.prototype.fetch = function (input: RequestInfo, init?: RequestInit) {
  return fetchWithSession(input, init, this);
};

const setDevicePermissionHandler = Session.prototype.setDevicePermissionHandler;

Session.prototype.setDevicePermissionHandler = function (handler) {
  if (handler) {
    setDevicePermissionHandler.call(this, (details) => {
      if (details.deviceType === 'serial') {
        const device = details.device as Electron.SerialPort;

        // Warn if the old (undocumented) snake_case names are used
        if (process.platform === 'win32') deprecate.renameProperty(device, 'device_instance_id', 'deviceInstanceId');
        deprecate.renameProperty(device, 'serial_number', 'serialNumber');
        if (process.platform === 'darwin') deprecate.renameProperty(device, 'usb_driver', 'usbDriverName');

        // The snake_case value for these props are numbers, but they're documented to be
        // strings and in other cases (like `select-serial-port`) they are, so normalize
        deprecate.renameProperty(device, 'product_id', 'productId', { normalize: { get: (val) => val.toString() } });
        deprecate.renameProperty(device, 'vendor_id', 'vendorId', { normalize: { get: (val) => val.toString() } });

        // This is not perfect, the old behavior was 'name' either mapped to 'displayName' or 'portName',
        // in that order of preference, so there's not actually a 1:1 mapping to rename here, unfortunately
        deprecate.renameProperty(device, 'name', 'portName');
      }

      return handler(details);
    });
  } else {
    setDevicePermissionHandler.call(this, null);
  }
};

export default {
  fromPartition,
  fromPath,
  get defaultSession () {
    return fromPartition('');
  }
};
