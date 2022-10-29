setlocal

call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars32.bat"


if /I [%1] == [rebuild] (
	set option="-t:Rebuild"
)

msbuild squeeze2upnp.sln /property:Configuration=Release %option%
msbuild squeeze2upnp.sln /property:Configuration=Static %option%

endlocal