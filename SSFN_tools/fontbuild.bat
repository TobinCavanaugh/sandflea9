@REM otf2bdf -p 12 -r 96 -c M -o FSEX302.bdf FSEX302.ttf

pushd "%~dp0"
del "../src/blob/regularfont.sfn"
sfnconv.exe -U "FSEX302.bdf" "../src/blob/regularfont.sfn"
popd
