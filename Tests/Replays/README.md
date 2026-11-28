# Check 60 Hz replay determinism

`linux_60hz_tournament_desert.rep` records a 739-tick Zero Hour skirmish on Tournament Desert. It was recorded on Linux x86-64 with the `GX60Z4` replay signature. At frame 89 the player orders a construction dozer to move. The recording uses Zero Hour 1.04 assets with INI checksum `0xDDF31B10` and map checksum `0x15F63DEA`.

Build with `-DRTS_EXPERIMENTAL_60HZ_SIMULATION=ON`. Launch the resulting Zero Hour executable from a working game installation. On Windows, place the executable alongside the game assets before running it. Pass the absolute path to the fixture.

```sh
./GeneralsXZH -headless -replay /path/to/Tests/Replays/linux_60hz_tournament_desert.rep
```

A successful run exits with code 0 and reports `Game Time: 00:12/00:12`. The first printed checksum comparison is frame 200. Strict playback checks later frames as well and returns a failure on a mismatch. An independent Windows x86 recording of the same match produced the same four checksums.

| Simulation frame | Recorded CRC |
| ---: | --- |
| 0 | `0xC195E560` |
| 200 | `0x1234D3AD` |
| 400 | `0x1797E71F` |
| 600 | `0xFB6882AC` |
