# SDK patch

The stock `framework/mpc_plugin.h` (mpc-plugin-builder-sdk, commit febd2ca) drops sysex events. `mpc_plugin_sysex.patch` adds a `sysexReceived(data, size, sampleOffset)` virtual that instruments can override. It is called before `process()` and the bytes are only valid during the call. `mpc_plugin.h` is the patched file in full.

```sh
cd <sdk> && git apply <this repo>/sdk-patch/mpc_plugin_sysex.patch
```
