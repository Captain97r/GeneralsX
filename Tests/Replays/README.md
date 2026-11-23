# Native 60 Hz Zero Hour replay

`windows_60hz_tournament_desert.rep` records a 739-frame skirmish on Tournament Desert with a human player and an easy AI opponent. The player selects a dozer and issues a move order on frame 89. The recording contains checksums for simulation frames 0, 200, 400 and 600.

Run the fixture from a Zero Hour 1.04 installation with the branch executable and the path to the recording.

```powershell
.\GeneralsXZH.exe -headless -replay "C:\path\to\windows_60hz_tournament_desert.rep"
```

A successful run exits with code 0 and reports `Game Time: 00:12/00:12`. The recording has an INI checksum of `0xDDF31B10`. The public Linux and macOS retail recordings have an INI checksum of `0xFEAAE3F3`. To exercise the native fixture in the existing replay CI jobs with matching assets, set the repository variable `TEST_NATIVE_60HZ_REPLAY` to `true`.
