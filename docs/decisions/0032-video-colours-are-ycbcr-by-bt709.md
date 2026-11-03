# 0032: A record's colours are YCbCr by BT.709, and the file says so

**Status:** proposed

## Context

[0031](0031-video-is-recorded-by-ffmpeg.md) records H.264 in 4:2:0, or lossless H.264 of the RGB
pixels (`libx264rgb`). QuickTime shows both in wrong colours:

- The lossless record keeps the planes G, B and R. The file says so, but AVFoundation decodes them
  as Y, Cb and Cr by BT.601. A frame of Mario's head came out all green. ffmpeg, VLC and mpv show
  it right.
- ffmpeg converts RGB to YCbCr by BT.601 and writes no matrix into the file. A player that is not
  told guesses. AVFoundation takes BT.709 for a large picture, such as the 1380 by 568 a `crisp`
  record makes: green 0,255,0 shows as 0,215,0, red 255,0,0 as 255,24,0.

AVFoundation decodes neither PNG nor QuickTime Animation in a MOV ("Cannot Decode"). It decodes
H.264 4:4:4 of 8 and of 10 bits, and follows the matrix and the range the file names.

## Decision

- **Every record is YCbCr by BT.709, limited range, and the file names the matrix and the range.**
  ffmpeg converts with `accurate_rnd`, `full_chroma_int` and `bitexact`.
- **The lossless record is H.264 4:4:4 of 10 bits, lossless.** Ten bits keep every RGB colour:
  of the 16,777,216 colours, converted and encoded by ffmpeg and decoded and converted back by it
  with the same flags, none differs. In 8 bits, colours come back up to 3 apart. With full range in
  10 bits, ffmpeg brings back 69% of them 1 apart.
- The RGB planes are not kept as a choice. No player shows them better than these.

## Consequences

- QuickTime shows both records in the right colours. AVFoundation's own conversion to 8-bit RGB
  puts about half the colours of a lossless record 1 apart. That is the player, not the file.
- The exact frames come back with ffmpeg only when it is told the matrix and the flags:
  `-vf scale=in_color_matrix=bt709:in_range=limited:flags=accurate_rnd+full_chroma_int+bitexact,format=rgb24`.
  Without the flags, colours come back 1 apart.
- The lossless record needs an ffmpeg whose libx264 encodes 10 bits. Builds of x264 from 2021 on
  encode 8 and 10 bits in one library.
- Browsers may not play H.264 4:4:4, as before.
