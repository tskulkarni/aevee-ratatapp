//
// The contents of this file are subject to RAAT SDK License
// agreement; You may not use this file except in compliance
// with the License.
//
// Copyright (C) 2015 Roon Labs LLC
//
// All Rights Reserved.
//
#error this file is only for the benefit of Doxygen. It's not meant to be actually compiled

/** \defgroup signal_path Signal Path
 *
 *  @{
 *  
 *  Roon's Signal Path feature is an important part of the audiophile listening experience. It creates and atmosphere
 *  of trust and transparency with our users, and highlights exactly what we are (and aren't!) doing to their audio.
 *
 *  One of the major goals of RAAT is to extend this feature into the hardware itself and provide more exposure into what
 *  is going on. This is an opportunity to show off the value-add (or bit-perfect transparency) of hardware products.
 *
 * An \ref raat_plugin_output can update the in-app signal path by emitting a message that contains the relevant information.
 *
 * That message looks like this:
 *
 * <pre>
 * { 
 *     "signal_path": [
 *         ... elements ...
 *     ]
 * }
 * </pre>
 *
 * Currently, Roon recognizes the following signal path elements. If your product does other things, reach out to your Roon
 * contact and we'll figure out how to reflect them in the app.
 *
 * <pre>
 * { "quality": "high",     "type": "volume_normalization",       "gain": -12.0 }
 * { "quality": "high",     "type": "digital_volume",             "gain": -12.0 }
 * { "quality": "lossless", "type": "analog_volume",              "gain": -22.2 }
 * { "quality": "lossless", "type": "dsd_encapsulate",            "method": "dop" | "dcs" }
 * { "quality": "high",     "type": "dsd_to_pcm",                 "from_sample_rate": 2822400, "to_sample_rate": 352800 }
 * { "quality": "high",     "type": "pcm_to_dsd",                 "from_sample_rate": 44100, "to_sample_rate": 5644800 }
 * { "quality": "high",     "type": "pcm_sample_rate_conversion", "from_sample_rate": 44100, "to_sample_rate": 384000 }
 * { "quality": "high",     "type": "dsd_sample_rate_conversion", "from_sample_rate": 2822400, "to_sample_rate": 5644800 }
 * { "quality": "lossless", "type": "amplifier",                  "method": "analog" | "digital" },
 * { "quality": "enhanced", "type": "eq",                         "sub_type": "bass" | "treble" | "tilt" | "bass_management", "gain": -2.0 },
 * { "quality": "lossless", "type": "invert_phase" },
 * { "quality": "lossless", "type": "balance",                    "value": -1.0<->1.0 | "-100<->100" },
 * { "quality": "lossless", "type": "output",                     "method": "alsa" | "other" | "usb" | "i2s" | "analog" | "aes" | "digital" | "asio" | "speakers" | "analog_digital" ... }
 * { "quality": "enhanced", "type": "audio_distribution", "method": "talk_to_roon_to_figure_out_what_to_put_here" },
 * { "quality": "enhanced", "type":                 "mqa", 
 *                          "sub_type":             "core_decoder" | "renderer",
 *                          "light_state":          "off" | "valid" | "authored" | "test_stream",
 *                          "original_sample_rate": "352800",
 *                          "quality":              "enhanced" }
 * </pre>
 *
 * @} */

/** \defgroup raatool raatool
 *
 * @{
 *  
 * raatool is a command line utility that you will probably run on your development machine. It understands how 
 * to speak to RAAT devices without using the full Roon application.
 *
 * <h2>Usage</h2>
 * <pre>
 *
 * USAGE raatool [globaloptions] &lt;command&gt; [options]
 *
 * GLOBAL OPTIONS
 *    --verbose,-v                     make responses more verbose
 *
 * COMMANDS
 *
 *    list                            list devices on the local LAN
 *    version                         print out the verison of raatool that's running, then exit
 *    info &lt;device id&gt;                display the info dictionary for the specified device id
 *    logdump &lt;device id&gt;             dump log to stdout
 *    logcat &lt;device id&gt;              tail the log to stdout
 *    discovery                       dump discovery packets on the network passively
 *    discoveryquery                  send a query, then dump discovery packets on the network passively
 *    run &lt;device id&gt; &lt;script&gt;        run a lua script on the device, redirecting output back to raatool
 *  </pre>
 *
 * @} */

/** \defgroup raat_app raat_app

raat_app is a driver that uses a JSON-based configuration file to start up a RAAT endpoint.

Depending on your situation, it might make sense to use it directly, with or without your own 
SDK modifications.

We've had many questions about the JSON configuration options supported by raat_app, so here 
are some samples:

## Sample #1

Notes:

- Output device is present at hw:1
- Supports DSD up to DSD256 (provided the connected hardware can do it)
- Prefers to use ALSA native DSD support when available, otherwise falls back on DoP
- Use the first volume/mute controls found in the ALSA mixer for hw:1 for volume control
- Exits raat_app if the device disappears
- This product exposes a configuration URL on port 8080
- This is a good starting point for bridge devices that have a USB port 
- Displays in signal path as "USB output" instead of the default "ALSA"

<pre>
{
    "vendor":    "Roon",
    "model":     "SAMPLE",
    "serial":    "123456",           
    "version":   "X.XX",
    "config_url":"http://__SELF__:8080/path/to/config/ui"
    "unique_id": "894122e8-4507-4b22-b93d-bc231483cb6d",
    "output": { 
        "type":            "alsa", 
        "device":          "hw:1,0",
        "dsd_mode":        "native_or_dop",             
        "max_dsd_rate":    256,
        "signal_path":     [ { "quality": "lossless", "type": "output", "method": "usb" } ],
    },
    "volume": { 
        "type":         "alsa",
        "device":       "hw:1",
    },
    "watch": {
        "type":       "alsa",
        "device":      "hw:1",
        "lost_action": "exit"
    }
}
</pre>

## Sample #2

Notes:

- Output device is present at hw:1
- PCM-only, up to 192k
- Uses software-based volume attenuation for volume adjustments
- Good for a networked DAC--assumes that the device will never disappear after RAAT starts up
- Displays in signal path as "Analog Output"  instead of the default "ALSA"

<pre>
{
    "vendor":    "Roon",
    "model":     "SAMPLE",
    "serial":    "123456",           
    "version":   "X.XX",
    "unique_id": "894122e8-4507-4b22-b93d-bc231483cb6d",
    "output": { 
        "type":            "alsa", 
        "device":          "hw:1,0",
        "signal_path":     [ { "quality": "lossless", "type": "output", "method": "analog" } ],
    },
    "volume": { 
        "type":         "software",
    },
}
</pre>

## Sample #3

Notes:

- Output device is present at hw:1
- DSD supported up to DSD128
- Prefers to use DoP for DSD output when possible, but will use Native if DoP isn't possible
- No volume control.
- Displays in signal path as "Digital Output" instead of the default "ALSA"

<pre>
{
    "vendor":    "Roon",
    "model":     "SAMPLE",
    "serial":    "123456",           
    "version":   "X.XX",
    "unique_id": "894122e8-4507-4b22-b93d-bc231483cb6d",
    "output": { 
        "type":            "alsa", 
        "device":          "hw:1,0",
        "dsd_mode":        "native_or_dop",             
        "max_dsd_rate":    128,
        "signal_path":     [ { "quality": "lossless", "type": "output", "method": "digital" } ],
    },
}
</pre>

## dB/Fractional Volume Control using ALSA 

Many USB DACs do not report their dB-based volume ranges correctly, so we do not recommend doing a dB-based
volume control on bridge devices that plug into unknown DACs.

For a networked DAC that displays dB on its front-panel, these settings should allow you to exactly
mirror the display values in Roon.

In the following example, volume ranges from -80dB to 0dB in 0.5dB steps:

<pre>
{
    ...
    "volume": { 
        "type":         "alsa",
        "device":       "hw:1",
        "mode":         "db",
        "db_min":       -80,
        "db_max":       0,
        "db_step":      0.5
    },
}
</pre>


## Transport and Source Selection Plugins 

If you add your own transport or source selection plugin, you'll want to append  stuff like 
this to the configuration:

<pre>
{
    ...
    "transport":        { "type": "my_type", ... }
    "source_selection": { "type": "my_type", ... }
}
</pre>

 * @} */

