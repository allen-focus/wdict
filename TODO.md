Use ~~x~~ to mark item as completed.

1. ~~Simple without fit~~
2. ~~Fit (x axis only)~~
3. ~~Fit~~
4. ~~Simple grow~~
5. ~~Multiple grow~~
6. ~~Shrink~~
7. ~~Text Wrap~~
8. Text Wrap fit (adjust pipeline)
9. Minimum/maximum size
10. Alignment

---

```chapters.txt
;FFMETADATA1
[CHAPTER]
TIMEBASE=1/1000
START=0
END=390000
title=Intro

[CHAPTER]
TIMEBASE=1/1000
START=390000
END=604000
title=Simple without fit

[CHAPTER]
TIMEBASE=1/1000
START=604000
END=844000
title=Fit (x axis only)

[CHAPTER]
TIMEBASE=1/1000
START=844000
END=1073000
title=Fit

[CHAPTER]
TIMEBASE=1/1000
START=1073000
END=1231000
title=Summary 1

[CHAPTER]
TIMEBASE=1/1000
START=1231000
END=1462000
title=Simple grow

[CHAPTER]
TIMEBASE=1/1000
START=1462000
END=1667000
title=Multiple grow

[CHAPTER]
TIMEBASE=1/1000
START=1667000
END=1764000
title=Text problem

[CHAPTER]
TIMEBASE=1/1000
START=1764000
END=2083000
title=Shrink

[CHAPTER]
TIMEBASE=1/1000
START=2083000
END=2139000
title=Text wrap

[CHAPTER]
TIMEBASE=1/1000
START=2139000
END=2232000
title=Text wrap fit (adjust pipeline)

[CHAPTER]
TIMEBASE=1/1000
START=2232000
END=2313000
title=Minimum/maximum size

[CHAPTER]
TIMEBASE=1/1000
START=2313000
END=2446000
title=Alignment

[CHAPTER]
TIMEBASE=1/1000
START=2446000
END=2573000
title=Summary 2
```

```
ffmpeg -y -i "How Clay's UI Layout Algorithm Works [by9lQvpvMIc].mkv" -i chapters.txt -map_metadata 1 -codec copy output.mkv
```
