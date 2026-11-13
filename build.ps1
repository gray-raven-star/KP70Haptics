$ErrorActionPreference = 'Stop'
Push-Location -LiteralPath $PSScriptRoot
try {
    zig c++ KP70Engine.cpp -o KP70HapticsEngine.exe -lole32 -luuid -lsetupapi -lcfgmgr32 -lhid -lxinput1_4 -O2 -std=c++17
    if ($LASTEXITCODE -ne 0) { throw 'Native engine build failed.' }

    dotnet publish app/KP70Studio.csproj -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true -p:IncludeNativeLibrariesForSelfExtract=true -p:EnableCompressionInSingleFile=true -o out
    if ($LASTEXITCODE -ne 0) { throw 'App publish failed.' }
}
finally {
    Pop-Location
}
