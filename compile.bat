call "%ProgramFiles%\Microsoft Visual Studio\2022\Enterprise\VC\Auxiliary\Build\vcvars64.bat"
msbuild "PCSX2_qt.sln" /m /v:m /p:Configuration=release
pause