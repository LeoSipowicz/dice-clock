    .section .rodata.frames, "a"
    .global frames_archive_start
    .global frames_archive_end
    .align 4
frames_archive_start:
    .incbin "/Users/leosipowicz/Desktop/code/c++/dice-clock/sketches/hx8357d-dice-clock/frames_320x480.lz4"
frames_archive_end:
