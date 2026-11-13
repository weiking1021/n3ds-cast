#!/usr/bin/env bash
set -euo pipefail
ulimit -c 0
cd "$(dirname "$0")"
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I. -I../shared test_frame.c frame.c -o /tmp/native_frame
/tmp/native_frame
gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I. test_discovery.c discovery.c -o /tmp/native_dns
/tmp/native_dns
gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I. -I../shared test_settings.c settings.c i18n.c ../shared/config.c -o /tmp/native_settings
/tmp/native_settings
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I. -I../shared test_video.c video.c rects.c frame.c ../shared/capture_jpeg.c -o /tmp/native_video
/tmp/native_video
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I. -I../shared test_game_audio.c game_audio.c ../shared/audio_stream.c -o /tmp/native_audio
/tmp/native_audio
python3 test_discovery_network.py
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I. test_i18n.c i18n.c -o /tmp/native_i18n
/tmp/native_i18n
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined test_ui_layout.c -o /tmp/native_ui_layout
/tmp/native_ui_layout
python3 preview_ui.py /tmp/native-ui-preview-images
# Programs that run on the console are compiled whole against a declaration-only <3ds.h> (test_stubs/); the test
# defines the kernel calls and display registers they use.
gcc -std=c11 -D__3DS__ -Itest_stubs -I. -I../shared -Wno-int-to-pointer-cast -Wall -Wextra -Werror -fsanitize=address,undefined test_capture.c capture.c frame.c -o /tmp/native_capture
/tmp/native_capture
gcc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I. test_overlay.c overlay_pixels.c frame.c -o /tmp/native_overlay_pixels
/tmp/native_overlay_pixels
gcc -std=c11 -D__3DS__ -Itest_stubs -I. -I../shared -Wno-int-to-pointer-cast -Wall -Wextra -Werror -fsanitize=address,undefined test_overlay_backend.c overlay.c overlay_pixels.c frame.c -o /tmp/native_overlay
/tmp/native_overlay
python3 test_launcher.py
gcc -std=gnu11 -D__3DS__ -Itest_stubs -I. -I../shared -Wno-int-to-pointer-cast -Wall -Wextra -Werror -fsanitize=address,undefined -I/opt/devkitpro/portlibs/3ds/include test_control.c service_state.c service_control.c -o /tmp/native_control
/tmp/native_control
# The real Cast client against a scripted receiver (socket calls renamed to stubs, mbedtls replaced by a fake pipe)
gcc -std=gnu11 -Wall -Wextra -Werror -fsanitize=address,undefined -I../shared -I/opt/devkitpro/portlibs/3ds/include     -Dsocket=stub_socket -Dconnect=stub_connect -Dpoll=stub_poll -Dfcntl=stub_fcntl -Dsetsockopt=stub_setsockopt     -Dclose=stub_close -Dusleep=stub_usleep     test_cast_client.c ../shared/cast_client.c ../shared/cast_proto.c ../shared/capture_jpeg.c -o /tmp/native_cast_client
/tmp/native_cast_client
# JPEG encoder must stay byte-identical to the reference implementation
gcc -O2 -std=c11 -Wall -Wextra -I../shared -I../tests ../tests/test_jpeg_exact.c ../shared/capture_jpeg.c ../tests/ref_capture_jpeg.c -o /tmp/native_jpeg_exact
/tmp/native_jpeg_exact
# Partial updates: rectangle planner, rectangle encoder exactness, cache decisions
gcc -O2 -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I../shared -I. ../tests/test_rects.c rects.c video.c frame.c ../shared/capture_jpeg.c -o /tmp/native_rects
/tmp/native_rects
python3 verify.py
