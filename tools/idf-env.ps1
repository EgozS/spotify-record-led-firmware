$env:IDF_PATH='C:\esp\v6.1\esp-idf'; $env:IDF_TOOLS_PATH='C:\Espressif\tools'; $env:IDF_PYTHON_ENV_PATH='C:\Espressif\tools\python\v6.1\venv'; $env:ESP_IDF_VERSION='6.1'
$env:PATH = "C:\Espressif\tools\python\v6.1\venv\Scripts;C:\Espressif\tools\xtensa-esp-elf\esp-15.2.0_20251204\xtensa-esp-elf\bin;C:\Espressif\tools\cmake\4.0.3\bin;C:\Espressif\tools\ninja\1.12.1;C:\Espressif\tools\ccache\4.12.1\ccache-4.12.1-windows-x86_64;" + $env:PATH
$env:ESP_ROM_ELF_DIR=(Get-ChildItem C:\Espressif\tools\esp-rom-elfs -Directory | Select -First 1).FullName
