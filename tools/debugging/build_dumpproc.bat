call "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\Common7\Tools\VsDevCmd.bat" -arch=x64 -host_arch=x64 >nul
cl /nologo /EHsc /Fe:"C:\Users\alex0\PycharmProjects\smash-classic\tools\debugging\dump_proc.exe" "C:\Users\alex0\PycharmProjects\smash-classic\tools\debugging\dump_proc.cpp" /link dbghelp.lib
