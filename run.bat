@echo off
REM ESP32 WiFi Provisioning - Windows Build, Flash, and Monitor Script
REM One-click solution with automatic dependency installation

setlocal enabledelayedexpansion

REM ============================================
REM Configuration
REM ============================================
set "DEFAULT_PORT=COM3"
set "AP_SSID=ESP32-Prov"
set "AP_PASSWORD=prov12345678"
set "ESP_IP=192.168.4.1"
set "PROJECT_DIR=%~dp0"
set "TARGET_CHIP=esp32s3"
set "IDF_VERSION=5.1"
set "TEMP_DIR=%TEMP%\esp32_setup"
set "INSTALLER_URL=https://dl.espressif.com/dl/esp-idf/esp-idf-tools-setup-online.exe"

REM ============================================
REM Helper Functions
REM ============================================
goto :main

:print_step
echo.
echo [STEP] %~1
goto :eof

:print_info
echo [INFO] %~1
goto :eof

:print_error
echo [ERROR] %~1
goto :eof

:print_success
echo [SUCCESS] %~1
goto :eof

:print_warning
echo [WARNING] %~1
goto :eof

:print_prompt
echo [PROMPT] %~1
goto :eof

REM ============================================
REM Check for Python
REM ============================================
:check_python
call :print_step "Checking Python installation"

where python >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    python --version >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        for /f "tokens=2" %%i in ('python --version 2^>^&1') do set "PYTHON_VERSION=%%i"
        call :print_success "Python found: !PYTHON_VERSION!"
        set "PYTHON_FOUND=1"
        goto :eof
    )
)

call :print_error "Python not found!"
call :print_info "Python is required for ESP-IDF"
call :print_info "Download from: https://www.python.org/downloads/"
call :print_info "Make sure to check 'Add Python to PATH' during installation"
set "PYTHON_FOUND=0"
goto :eof

REM ============================================
REM Check for Git
REM ============================================
:check_git
call :print_step "Checking Git installation"

where git >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    for /f "tokens=3" %%i in ('git --version 2^>^&1') do set "GIT_VERSION=%%i"
    call :print_success "Git found: !GIT_VERSION!"
    set "GIT_FOUND=1"
    goto :eof
)

call :print_error "Git not found!"
call :print_info "Git is required for ESP-IDF"
call :print_info "Download from: https://git-scm.com/download/win"
set "GIT_FOUND=0"
goto :eof

REM ============================================
REM Check for ESP-IDF
REM ============================================
:check_esp_idf
call :print_step "Checking ESP-IDF installation"

REM Check for IDF_PATH environment variable
if defined IDF_PATH (
    if exist "%IDF_PATH%\export.bat" (
        call :print_success "ESP-IDF found at: %IDF_PATH%"
        set "IDF_FOUND=1"
        goto :eof
    )
)

REM Check for idf.py in PATH
where idf.py >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    call :print_success "ESP-IDF found (idf.py in PATH)"
    set "IDF_FOUND=1"
    goto :eof
)

REM Check common installation locations
if exist "C:\Espressif\frameworks\esp-idf-v5.1\export.bat" (
    call :print_info "Found ESP-IDF at: C:\Espressif\frameworks\esp-idf-v5.1"
    set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.1"
    set "IDF_FOUND=1"
    goto :eof
)

if exist "C:\Espressif\frameworks\esp-idf-v5.0\export.bat" (
    call :print_info "Found ESP-IDF at: C:\Espressif\frameworks\esp-idf-v5.0"
    set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.0"
    set "IDF_FOUND=1"
    goto :eof
)

if exist "%USERPROFILE%\esp\esp-idf\export.bat" (
    call :print_info "Found ESP-IDF at: %USERPROFILE%\esp\esp-idf"
    set "IDF_PATH=%USERPROFILE%\esp\esp-idf"
    set "IDF_FOUND=1"
    goto :eof
)

call :print_error "ESP-IDF not found!"
set "IDF_FOUND=0"
goto :eof

REM ============================================
REM Download ESP-IDF Installer
REM ============================================
:download_installer
call :print_step "Downloading ESP-IDF Online Installer"

REM Create temp directory
if not exist "%TEMP_DIR%" mkdir "%TEMP_DIR%"

set "INSTALLER_PATH=%TEMP_DIR%\esp-idf-tools-setup-online.exe"

REM Check if already downloaded
if exist "%INSTALLER_PATH%" (
    call :print_info "Installer already exists, skipping download"
    set "DOWNLOAD_SUCCESS=1"
    goto :eof
)

call :print_info "Downloading installer from Espressif..."
call :print_info "This may take a few minutes (~4 MB download)"

set "DOWNLOAD_SUCCESS=0"

REM Try PowerShell download (Windows 7+)
powershell -Command "& {[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12; Invoke-WebRequest -Uri '%INSTALLER_URL%' -OutFile '%INSTALLER_PATH%'}" >nul 2>&1

if %ERRORLEVEL% EQU 0 (
    if exist "%INSTALLER_PATH%" (
        call :print_success "Installer downloaded successfully"
        set "DOWNLOAD_SUCCESS=1"
        goto :eof
    )
)

REM Fallback: Try curl if available
where curl >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    curl -L -o "%INSTALLER_PATH%" "%INSTALLER_URL%" >nul 2>&1
    if %ERRORLEVEL% EQU 0 (
        if exist "%INSTALLER_PATH%" (
            call :print_success "Installer downloaded successfully"
            set "DOWNLOAD_SUCCESS=1"
            goto :eof
        )
    )
)

call :print_error "Failed to download installer automatically"
call :print_info "Please download manually from:"
call :print_info "  %INSTALLER_URL%"
call :print_info "Save it to: %INSTALLER_PATH%"
set "DOWNLOAD_SUCCESS=0"
goto :eof

REM ============================================
REM Launch ESP-IDF Installer
REM ============================================
:launch_installer
call :print_step "Launching ESP-IDF Installer"

set "INSTALLER_PATH=%TEMP_DIR%\esp-idf-tools-setup-online.exe"

if not exist "%INSTALLER_PATH%" (
    call :print_error "Installer not found at: %INSTALLER_PATH%"
    call :print_info "Attempting to download..."
    call :download_installer
    if !DOWNLOAD_SUCCESS! EQU 0 (
        exit /b 1
    )
)

call :print_info "Starting ESP-IDF installer..."
call :print_info "Please follow the installer prompts:"
call :print_info "  1. Accept the license agreement"
call :print_info "  2. Select ESP-IDF version %IDF_VERSION% (or latest)"
call :print_info "  3. Choose installation directory (default is fine)"
call :print_info "  4. Wait for installation to complete"
echo.
call :print_prompt "Press any key to launch the installer..."
pause >nul

start "" "%INSTALLER_PATH%"

call :print_info "Installer launched. Waiting for installation to complete..."
call :print_info "This may take 10-30 minutes depending on your internet speed."
echo.
call :print_prompt "After installation completes, press any key to continue..."
pause >nul

REM Check if installation succeeded - verify idf.py exists
call :print_info "Verifying ESP-IDF installation..."
timeout /t 3 /nobreak >nul

REM Check common installation paths for idf.py
set "IDF_VERIFIED=0"

REM Check if idf.py is now in PATH
where idf.py >nul 2>&1
if %ERRORLEVEL% EQU 0 (
    set "IDF_VERIFIED=1"
    call :print_success "ESP-IDF installation verified (idf.py in PATH)"
    goto :verify_done
)

REM Check common installation locations
if exist "C:\Espressif\frameworks\esp-idf-v5.1\tools\idf.py" (
    set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.1"
    set "IDF_VERIFIED=1"
    call :print_success "ESP-IDF installation verified at: %IDF_PATH%"
    goto :verify_done
)

if exist "C:\Espressif\frameworks\esp-idf-v5.0\tools\idf.py" (
    set "IDF_PATH=C:\Espressif\frameworks\esp-idf-v5.0"
    set "IDF_VERIFIED=1"
    call :print_success "ESP-IDF installation verified at: %IDF_PATH%"
    goto :verify_done
)

if exist "%USERPROFILE%\esp\esp-idf\tools\idf.py" (
    set "IDF_PATH=%USERPROFILE%\esp\esp-idf"
    set "IDF_VERIFIED=1"
    call :print_success "ESP-IDF installation verified at: %IDF_PATH%"
    goto :verify_done
)

:verify_done
if !IDF_VERIFIED! EQU 0 (
    call :print_warning "ESP-IDF installation may not be complete"
    call :print_info "Please ensure the installer finished successfully"
    call :print_info "You may need to restart this script after installation"
    call :print_info "Or restart your command prompt to refresh environment variables"
    exit /b 1
)

call :print_success "ESP-IDF installation verified!"
goto :eof

REM ============================================
REM Setup ESP-IDF Environment
REM ============================================
:setup_esp_idf
call :print_step "Setting up ESP-IDF environment"

REM If IDF_PATH not set, try to find it
if not defined IDF_PATH (
    call :check_esp_idf
    if !IDF_FOUND! EQU 0 (
        call :print_error "IDF_PATH not set and ESP-IDF not found"
        call :print_info "Please install ESP-IDF first or set IDF_PATH environment variable"
        exit /b 1
    )
)

REM Verify IDF_PATH is set and valid
if not defined IDF_PATH (
    call :print_error "IDF_PATH is not set"
    exit /b 1
)

if not exist "%IDF_PATH%\export.bat" (
    call :print_error "export.bat not found at: %IDF_PATH%"
    call :print_info "ESP-IDF may not be installed correctly"
    exit /b 1
)

REM Source ESP-IDF environment
call "%IDF_PATH%\export.bat" >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    call :print_error "Failed to setup ESP-IDF environment"
    call :print_info "Try running: %IDF_PATH%\export.bat manually"
    exit /b 1
)

REM Verify idf.py is now available
where idf.py >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    call :print_warning "idf.py not found in PATH after export"
    call :print_info "ESP-IDF environment may not be set up correctly"
    call :print_info "Try restarting your command prompt"
    exit /b 1
)

call :print_success "ESP-IDF environment loaded"
goto :eof

REM ============================================
REM Check USB Drivers
REM ============================================
:check_usb_drivers
call :print_step "Checking USB-to-Serial drivers"

REM Try to find any COM ports
set "PORTS_FOUND=0"
for /L %%i in (1,1,20) do (
    if exist "\\.\COM%%i" (
        set "PORTS_FOUND=1"
        goto :driver_check_done
    )
)

:driver_check_done
if !PORTS_FOUND! EQU 0 (
    call :print_warning "No COM ports detected"
    call :print_info "This might mean:"
    call :print_info "  1. ESP32 board is not connected"
    call :print_info "  2. USB-to-Serial driver is not installed"
    echo.
    call :print_info "Common drivers needed:"
    call :print_info "  - CP210x: https://www.silabs.com/developers/usb-to-uart-bridge-vcp-drivers"
    call :print_info "  - CH340/CH341: Search for 'CH340 driver Windows'"
    echo.
    call :print_prompt "Press any key to continue anyway..."
    pause >nul
) else (
    call :print_success "COM ports detected (drivers likely installed)"
)
goto :eof

REM ============================================
REM Find COM Port
REM ============================================
:find_com_port
call :print_step "Finding ESP32 COM port"

if not "%~1"=="" (
    set "ESP_PORT=%~1"
    call :print_info "Using provided port: %ESP_PORT%"
    goto :check_port_exists
)

call :print_info "Scanning for COM ports..."

if exist "\\.\%DEFAULT_PORT%" (
    set "ESP_PORT=%DEFAULT_PORT%"
    call :print_success "Found ESP32 at: %ESP_PORT%"
    goto :eof
)

set "PORT_FOUND=0"
for /L %%i in (1,1,20) do (
    if exist "\\.\COM%%i" (
        set "TEST_PORT=COM%%i"
        echo Found COM port: COM%%i
        set /p "CONFIRM=Use COM%%i? (Y/n): "
        if /i "!CONFIRM!"=="n" (
            set "CONFIRM="
        ) else (
            set "ESP_PORT=COM%%i"
            set "PORT_FOUND=1"
            call :print_success "Using port: COM%%i"
            goto :port_selected
        )
    )
)

:port_selected
if !PORT_FOUND! EQU 0 (
    call :print_error "No COM port selected or found!"
    call :print_info "Please connect ESP32 board and try again"
    call :print_info "Or specify port manually: run.bat COM3"
    set "ESP_PORT="
)
goto :eof

:check_port_exists
if "%ESP_PORT%"=="" (
    call :print_error "No port specified"
    goto :eof
)

if not exist "\\.\%ESP_PORT%" (
    call :print_error "Port %ESP_PORT% does not exist!"
    call :print_info "Available ports:"
    for /L %%i in (1,1,20) do (
        if exist "\\.\COM%%i" echo   COM%%i
    )
    set "ESP_PORT="
    goto :eof
)
goto :eof

REM ============================================
REM Set Target Chip
REM ============================================
:set_target
call :print_step "Setting target chip"

if exist "%PROJECT_DIR%\.idf_target" (
    for /f "delims=" %%i in ('type "%PROJECT_DIR%\.idf_target"') do set "CURRENT_TARGET=%%i"
    if "!CURRENT_TARGET!"=="%TARGET_CHIP%" (
        call :print_success "Target already set to %TARGET_CHIP%"
        goto :eof
    )
)

call :print_info "Setting target to %TARGET_CHIP%..."
idf.py set-target %TARGET_CHIP% >nul 2>&1
if %ERRORLEVEL% NEQ 0 (
    call :print_warning "Failed to set target (may already be set)"
) else (
    call :print_success "Target set to %TARGET_CHIP%"
)
goto :eof

REM ============================================
REM Build Project
REM ============================================
:build_project
call :print_step "Building project"

cd /d "%PROJECT_DIR%"
if %ERRORLEVEL% NEQ 0 (
    call :print_error "Failed to change to project directory"
    exit /b 1
)

if exist "build\wifi_ap_project.bin" (
    call :print_info "Previous build found. Rebuilding..."
)

call :print_info "Building firmware..."
idf.py build
if %ERRORLEVEL% NEQ 0 (
    call :print_error "Build failed!"
    exit /b 1
)

call :print_success "Build completed"
goto :eof

REM ============================================
REM Flash to ESP32
REM ============================================
:flash_esp32
call :print_step "Flashing firmware to ESP32"

if "%ESP_PORT%"=="" (
    call :print_error "ESP_PORT not set. Cannot flash."
    exit /b 1
)

cd /d "%PROJECT_DIR%"
if %ERRORLEVEL% NEQ 0 (
    call :print_error "Failed to change to project directory"
    exit /b 1
)

call :print_info "Flashing to %ESP_PORT%..."
idf.py -p %ESP_PORT% flash
if %ERRORLEVEL% NEQ 0 (
    call :print_error "Flash failed!"
    exit /b 1
)

call :print_success "Firmware flashed successfully"
goto :eof

REM ============================================
REM Monitor Serial Output
REM ============================================
:monitor_esp32
call :print_step "Starting serial monitor"

if "%ESP_PORT%"=="" (
    call :print_error "ESP_PORT not set. Cannot monitor."
    exit /b 1
)

cd /d "%PROJECT_DIR%"
if %ERRORLEVEL% NEQ 0 (
    call :print_error "Failed to change to project directory"
    exit /b 1
)

call :print_info "Press Ctrl+] to exit monitor"
call :print_info "Monitoring %ESP_PORT%..."
echo.

idf.py -p %ESP_PORT% monitor
goto :eof

REM ============================================
REM Install All Dependencies
REM ============================================
:install_dependencies
call :print_info "=== INSTALLING DEPENDENCIES ==="
call :print_info "This will check and install all required dependencies"
echo.

REM Check Python
call :check_python
if !PYTHON_FOUND! EQU 0 (
    call :print_error "Python is required but not found!"
    call :print_info "Please install Python first from: https://www.python.org/downloads/"
    call :print_info "Make sure to check 'Add Python to PATH' during installation"
    call :print_prompt "Press any key to open Python download page..."
    pause >nul
    start https://www.python.org/downloads/
    exit /b 1
)

REM Check Git
call :check_git
if !GIT_FOUND! EQU 0 (
    call :print_error "Git is required but not found!"
    call :print_info "Please install Git from: https://git-scm.com/download/win"
    call :print_prompt "Press any key to open Git download page..."
    pause >nul
    start https://git-scm.com/download/win
    exit /b 1
)

REM Check ESP-IDF
call :check_esp_idf
if !IDF_FOUND! EQU 0 (
    call :print_warning "ESP-IDF not found. Installing..."
    call :download_installer
    if !DOWNLOAD_SUCCESS! EQU 0 (
        call :print_error "Failed to download installer"
        exit /b 1
    )
    call :launch_installer
    if %ERRORLEVEL% NEQ 0 (
        call :print_error "ESP-IDF installation failed or incomplete"
        exit /b 1
    )
    REM Re-check after installation
    call :check_esp_idf
    if !IDF_FOUND! EQU 0 (
        call :print_error "ESP-IDF still not found after installation"
        call :print_info "You may need to restart this script or your command prompt"
        exit /b 1
    )
)

REM Check USB drivers
call :check_usb_drivers

call :print_success "All dependencies checked!"
goto :eof

REM ============================================
REM Quick Mode (Flash + Monitor only)
REM ============================================
:quick_mode
call :print_info "=== QUICK MODE ==="
call :print_info "Skipping setup checks. Just flashing and monitoring."
echo.

if not defined IDF_PATH (
    where idf.py >nul 2>&1
    if %ERRORLEVEL% NEQ 0 (
        call :print_error "ESP-IDF not found. Run with /setup flag for full setup."
        exit /b 1
    )
)

if "%~1"=="" (
    call :find_com_port
) else (
    set "ESP_PORT=%~1"
    call :check_port_exists
)

if "%ESP_PORT%"=="" (
    exit /b 1
)

call :build_project
if %ERRORLEVEL% NEQ 0 (
    exit /b 1
)

call :flash_esp32
if %ERRORLEVEL% NEQ 0 (
    exit /b 1
)

call :print_info "Waiting 3 seconds for device to boot..."
timeout /t 3 /nobreak >nul

call :monitor_esp32
goto :eof

REM ============================================
REM Full Setup Mode (One-Click Solution)
REM ============================================
:full_setup
call :print_info "=== ONE-CLICK SETUP MODE ==="
call :print_info "This will check and install all dependencies automatically"
echo.

REM Install all dependencies
call :install_dependencies
if %ERRORLEVEL% NEQ 0 (
    exit /b 1
)

REM Setup ESP-IDF environment
if defined IDF_PATH (
    where idf.py >nul 2>&1
    if %ERRORLEVEL% NEQ 0 (
        call :setup_esp_idf
        if %ERRORLEVEL% NEQ 0 (
            exit /b 1
        )
    ) else (
        call :print_success "ESP-IDF environment already active"
    )
) else (
    call :setup_esp_idf
    if %ERRORLEVEL% NEQ 0 (
        exit /b 1
    )
)

REM Find COM port
if "%~1"=="" (
    call :find_com_port
) else (
    set "ESP_PORT=%~1"
    call :check_port_exists
)

if "%ESP_PORT%"=="" (
    exit /b 1
)

REM Set target
call :set_target

REM Build
call :build_project
if %ERRORLEVEL% NEQ 0 (
    exit /b 1
)

REM Flash
call :flash_esp32
if %ERRORLEVEL% NEQ 0 (
    exit /b 1
)

REM Wait for boot
call :print_info "Waiting 3 seconds for device to boot..."
timeout /t 3 /nobreak >nul

REM Monitor
call :monitor_esp32
goto :eof

REM ============================================
REM Clean Build
REM ============================================
:clean_build
call :print_step "Cleaning build directory"

cd /d "%PROJECT_DIR%"
if %ERRORLEVEL% NEQ 0 (
    call :print_error "Failed to change to project directory"
    exit /b 1
)

idf.py fullclean
if %ERRORLEVEL% NEQ 0 (
    call :print_warning "Clean may have had issues, continuing anyway..."
) else (
    call :print_success "Build directory cleaned"
)
goto :eof

REM ============================================
REM Main Entry Point
REM ============================================
:main
echo ==========================================
echo ESP32 WiFi Provisioning - Windows Script
echo One-Click Setup Solution
echo ==========================================
echo.

if "%~1"=="" (
    call :full_setup
    goto :end
)

if /i "%~1"=="/setup" (
    shift
    call :full_setup %*
    goto :end
)

if /i "%~1"=="/quick" (
    shift
    call :quick_mode %*
    goto :end
)

if /i "%~1"=="/install" (
    call :install_dependencies
    goto :end
)

if /i "%~1"=="/clean" (
    call :clean_build
    if "%~2"=="" (
        goto :end
    )
    shift
    call :quick_mode %*
    goto :end
)

if /i "%~1"=="/help" (
    echo Usage:
    echo   run.bat              - One-click setup: installs dependencies and runs full setup
    echo   run.bat /setup       - Full setup mode (checks everything)
    echo   run.bat /quick COM3  - Quick mode: just flash and monitor (assumes setup done)
    echo   run.bat /install     - Install dependencies only (Python, Git, ESP-IDF)
    echo   run.bat /clean       - Clean build directory
    echo   run.bat /clean COM3  - Clean and then flash/monitor
    echo   run.bat /help        - Show this help
    echo.
    echo One-Click Mode (default):
    echo   - Checks for Python and Git
    echo   - Downloads and installs ESP-IDF if needed
    echo   - Sets up environment
    echo   - Builds, flashes, and monitors
    echo.
    echo Quick Mode:
    echo   - Assumes all dependencies are installed
    echo   - Just builds, flashes, and monitors
    goto :end
)

REM Assume port was provided for quick mode
call :quick_mode %*

:end
if %ERRORLEVEL% NEQ 0 (
    echo.
    echo Script failed with error code: %ERRORLEVEL%
    pause
)
endlocal
