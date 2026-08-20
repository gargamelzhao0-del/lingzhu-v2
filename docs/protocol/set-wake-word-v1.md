# Runtime Wake-Word Command v1

This contract is available when the device hello advertises
`features.remote_wake_word_config=true` (ESP-SR MultiNet backend). It changes
the active MultiNet speech command; it does not replace the acoustic model.

The preferred envelope includes `type` and `request_id`, as shown below. For
compatibility with the deployed cloud command shape, this command alone also
accepts an object containing only `command`, `wake_word`, and `persist`:

```json
{
  "command": "set_wake_word",
  "wake_word": [
    "ni hao ling zhu",
    "ni hao lin zu",
    "ni hao ling zu",
    "ni hao lin zhu"
  ],
  "persist": true
}
```

No other `device_command` may omit `type` or `request_id`. Because this legacy
shape has no correlation id, its acknowledgement omits `request_id`; retries
cannot be deduplicated. Cloud implementations should migrate to the preferred
envelope when possible.

## Cloud to Device

```json
{
  "type": "device_command",
  "request_id": "wake_001",
  "command": "set_wake_word",
  "wake_word": [
    "xiao zhu xiao zhu",
    "xiao zu xiao zu",
    "xiao zhu xiao zu",
    "xiao zu xiao zhu",
    "ni hao ling zhu",
    "ni hao lin zhu",
    "ni hao ling zu",
    "ni hao lin zu",
    "ni hao xiao zu",
    "ni hao xiao zhu"
  ],
  "persist": true
}
```

`wake_word` must be a non-empty array of pinyin strings. The device retains at
most the first 10 array items and ignores every later item. Each retained string
is 2 to 63 ASCII bytes and may contain letters, digits, and whitespace. The
firmware converts ASCII letters to lowercase, trims and collapses whitespace,
removes normalized duplicates while preserving order, and registers every
remaining item under the same MultiNet command id. MultiNet grammar support
still determines whether a particular pinyin sequence can be parsed. The
number of pinyin syllables inside each string is not fixed; different items in
the same array may contain different numbers of syllables.

For compatibility with older cloud versions, a single string is also accepted.
Any built-in factory phrase sent in the legacy single-string format expands to
the complete ten-phrase factory set. Array input is always a complete
replacement: after truncation, normalization, and duplicate removal, the old
command set is discarded and only the resulting array is registered. For
example, an eight-item array replaces an existing ten-item set with eight
items; a historical four-item array remains four items.

`persist` defaults to `true`. With `false`, the command lasts until reboot;
with `true`, it is committed to NVS and loaded on the next detector init.

## Device Acknowledgement

Success:

```json
{
  "type": "device_command_ack",
  "request_id": "wake_001",
  "command": "set_wake_word",
  "ok": true,
  "message": "wake_words=10 primary=xiao zhu xiao zhu persist=1 truncated=0"
}
```

`truncated=1` reports that the incoming array contained more than 10 items and
only the first 10 were retained. Failure returns `ok:false`. Missing, overlong,
non-ASCII, or unparseable retained commands are rejected. Items after the first
10 are not applied. If a runtime update fails after the old MultiNet tree
has been replaced, firmware restores the complete previous command set and its
previous persistent snapshot before restarting the detector. If restoration
cannot be proven, the detector remains stopped and the negative ACK is retained.

Persistent arrays are stored in a versioned NVS blob. The primary phrase is
also written to the legacy `command` key so a firmware downgrade can still boot
with a valid wake phrase. Existing single-string NVS values and four-slot v2
array blobs are loaded and migrated automatically; a stored four-item array
remains four items after firmware upgrade.

Changing the command while the wake detector is running briefly stops and
restarts that detector. The normal touch-to-wake path is unchanged.

## Reset to Firmware Default

```json
{
  "type": "device_command",
  "request_id": "wake_reset_001",
  "command": "reset_wake_word"
}
```

The reset takes effect immediately and restores the firmware default command
set. In the current firmware that set is:

```text
xiao zhu xiao zhu
xiao zu xiao zu
xiao zhu xiao zu
xiao zu xiao zhu
ni hao ling zhu
ni hao lin zhu
ni hao ling zu
ni hao lin zu
ni hao xiao zu
ni hao xiao zhu
```

On success, the device removes only `wake_word/config_v2` and the legacy
`wake_word/command` NVS entries. Wi-Fi credentials, device identity, cloud
binding, volume, and all other NVS data are preserved. The same runtime and
NVS snapshot rollback used by `set_wake_word` applies if reset, detector
restart, or storage commit fails.

Success acknowledgement:

```json
{
  "type": "device_command_ack",
  "request_id": "wake_reset_001",
  "command": "reset_wake_word",
  "ok": true,
  "message": "wake_words=10 source=factory_default"
}
```

## Threshold

The product default is `CONFIG_NUWAX_CUSTOM_WAKE_WORD_THRESHOLD_PERCENT=8`,
passed to MultiNet as `0.07`. Results below the `0.10` strong threshold must
also pass the device voice-span, SNR, and clipping checks. Field testing must
still measure false wakes in the target noise environment.
