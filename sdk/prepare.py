#!/usr/bin/env python3
"""Apply the codec integration to an isolated, pinned WebRTC SDK checkout."""
import argparse
from pathlib import Path
import re
import shutil
import subprocess

parser = argparse.ArgumentParser()
parser.add_argument('source', type=Path)
parser.add_argument('--ffmpeg-include', type=Path, default=Path('/usr/include/x86_64-linux-gnu'))
args = parser.parse_args()
source = args.source.resolve()
wrapper = source / 'libwebrtc'
root_build = source / 'BUILD.gn'
root_text = root_build.read_text()
if 'deps = [ ":webrtc", "//libwebrtc" ]' not in root_text:
    if 'deps = [ ":webrtc" ]' not in root_text:
        raise SystemExit('WebRTC root build changed; review default target')
    root_build.write_text(root_text.replace('deps = [ ":webrtc" ]', 'deps = [ ":webrtc", "//libwebrtc" ]', 1))
# This pinned engine revision's call/rtp_config.cc calls
# std::optional<Rtx>::emplace() with no arguments. Newer libstdc++ (observed
# on GCC 16) rejects that overload for this aggregate under
# std::is_constructible_v, even though it is default-constructible via normal
# initialization — this project deliberately builds against the system
# libstdc++ (use_custom_libcxx=false) rather than Chromium's bundled libc++,
# so it inherits whatever stdlib the host toolchain ships. Work around it with
# plain assignment, which sidesteps emplace()'s overload resolution entirely.
rtp_config = source / 'call/rtp_config.cc'
text = rtp_config.read_text()
old_rtx = 'auto& stream_config_rtx = stream_config.rtx.emplace();'
if old_rtx in text:
    rtp_config.write_text(text.replace(
        old_rtx,
        'stream_config.rtx = RtpStreamConfig::Rtx();\n'
        '    auto& stream_config_rtx = *stream_config.rtx;',
        1))
# Same story: libwebrtc/include/rtc_types.h uses uint32_t without including
# <cstdint>, relying on a transitive include that newer, leaner standard
# library headers no longer guarantee.
rtc_types = wrapper / 'include/rtc_types.h'
text = rtc_types.read_text()
guard = '#ifndef LIB_WEBRTC_RTC_TYPES_HXX\n#define LIB_WEBRTC_RTC_TYPES_HXX\n'
if guard in text and '#include <cstdint>' not in text:
    rtc_types.write_text(text.replace(guard, guard + '\n#include <cstdint>\n', 1))
include = args.ffmpeg_include.resolve()
majors = {}
for name in ('avcodec', 'avutil'):
    headers = '\n'.join(p.read_text() for p in (include / ('lib' + name)).glob('version*.h'))
    match = re.search(r'#define\s+LIB' + name.upper() + r'_VERSION_MAJOR\s+(\d+)', headers)
    if not match:
        raise SystemExit('Missing system FFmpeg headers: ' + str(include / ('lib' + name)))
    majors[name] = match.group(1)
patch = wrapper / 'patches/custom_audio_source_m144.patch'
if subprocess.run(['git', 'apply', '--check', str(patch)], cwd=source, capture_output=True).returncode == 0:
    subprocess.run(['git', 'apply', str(patch)], cwd=source, check=True)
else:
    subprocess.run(['git', 'apply', '--reverse', '--check', str(patch)], cwd=source, check=True)
for filename in ('system_video_codecs.cc', 'system_video_codecs.h'):
    shutil.copyfile(Path(__file__).parent / filename, wrapper / 'src' / filename)
factory = wrapper / 'src/rtc_peerconnection_factory_impl.cc'
text = factory.read_text()
if '#include "system_video_codecs.h"' not in text:
    text = '#include "system_video_codecs.h"\n' + text
    old = 'webrtc::CreateBuiltinVideoEncoderFactory(),\n        webrtc::CreateBuiltinVideoDecoderFactory(),'
    if old not in text:
        raise SystemExit('The SDK factory changed; review the integration patch')
    text = text.replace(old, 'compartilhagram::CreateSystemEncoderFactory(),\n        compartilhagram::CreateSystemDecoderFactory(),')
    factory.write_text(text)
build = wrapper / 'BUILD.gn'
text = build.read_text()
if 'src/system_video_codecs.cc' not in text:
    text = text.replace('  sources = [', '  sources = [\n    "src/system_video_codecs.cc",\n    "src/system_video_codecs.h",', 1)
    text = text.replace('  deps = [', '  deps = [\n    "//api/video_codecs:rtc_software_fallback_wrappers",\n    "//third_party/ffmpeg",', 1)
    build.write_text(text)
# All FFmpeg consumers must use the same system ABI. Linking Chromium's private
# FFmpeg and system FFmpeg into one SDK risks resolving calls to the wrong ABI.
(source / 'third_party/ffmpeg/BUILD.gn').write_text('''config("system_ffmpeg") {
  include_dirs = [ "%s" ]
  libs = [ ":libavcodec.so.%s", ":libavutil.so.%s" ]
}
source_set("ffmpeg") {
  public_configs = [ ":system_ffmpeg" ]
}
''' % (include, majors['avcodec'], majors['avutil']))
print('Prepared system-codec SDK at', source)
