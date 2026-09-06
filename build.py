#!/usr/bin/env python3
"""
made with love by santa
"""

import os
import sys
import subprocess
import shutil
import zipfile
import argparse
import urllib.request
import tempfile
from pathlib import Path
from typing import List, Dict, Optional, Tuple

class Colors:
    HEADER = '\033[95m'
    OKBLUE = '\033[94m'
    OKCYAN = '\033[96m'
    OKGREEN = '\033[92m'
    WARNING = '\033[93m'
    FAIL = '\033[91m'
    ENDC = '\033[0m'
    BOLD = '\033[1m'

def print_banner():
    print()
    print("    ####  #   #  ####   ####  #####   ####  ####  #####")
    print("   #      #   # #    #  #       #     #     #     #   #")
    print("   #  ### ##### #    #  ####    #     ####  ####  #####")
    print("   #   #  #   # #    #     #    #     #        #  #")
    print("    ####  #   #  ####   ####    #     ##### ####  #")
    print()
    print("       +======================================+")
    print("       |      Cross-Platform Build Script     |")
    print("       +======================================+")
    print()

def download_esp_idf(version: str = "6.0.2") -> Optional[str]:
    """Download and extract ESP-IDF"""
    print(f"\nDownloading ESP-IDF v{version}...")
    
    # ESP-IDF download URLs
    urls = {
        "6.0.2": "https://github.com/espressif/esp-idf/releases/download/v6.0.2/esp-idf-v6.0.2.zip",
        "5.5.1": "https://github.com/espressif/esp-idf/releases/download/v5.5.1/esp-idf-v5.5.1.zip",
        "5.5": "https://github.com/espressif/esp-idf/releases/download/v5.5/esp-idf-v5.5.zip",
        "5.4.1": "https://github.com/espressif/esp-idf/releases/download/v5.4.1/esp-idf-v5.4.1.zip"
    }
    
    if version not in urls:
        print(f"ERROR: Unsupported ESP-IDF version {version}")
        return None
    
    # Determine installation directory (in the same folder as the build script)
    script_dir = os.path.dirname(os.path.abspath(__file__))
    install_path = os.path.join(script_dir, f"esp-idf-v{version}")
    
    if os.path.exists(install_path):
        print(f"ESP-IDF v{version} already exists at {install_path}")
        return install_path
    
    try:
        # Create temporary file for download in the project directory
        script_dir = os.path.dirname(os.path.abspath(__file__))
        tmp_fd, tmp_path = tempfile.mkstemp(suffix='.zip', dir=script_dir)
        try:
            os.close(tmp_fd)  # Close the file descriptor immediately
            
            print(f"Downloading from {urls[version]}...")
            
            def progress_hook(block_num, block_size, total_size):
                downloaded = block_num * block_size
                if total_size > 0:
                    percent = min(100, (downloaded * 100) // total_size)
                    print(f"\rProgress: {percent}% ({downloaded // 1024 // 1024} MB / {total_size // 1024 // 1024} MB)", end="")
            
            urllib.request.urlretrieve(urls[version], tmp_path, progress_hook)
            print()  # New line after progress
            
            # Extract ZIP file
            print(f"Extracting to {install_path}...")
            with zipfile.ZipFile(tmp_path, 'r') as zip_ref:
                # Extract to temporary directory first (use project directory for temp)
                temp_extract_dir = os.path.join(script_dir, f"temp_extract_{version}")
                try:
                    if os.path.exists(temp_extract_dir):
                        shutil.rmtree(temp_extract_dir)
                    os.makedirs(temp_extract_dir)
                    
                    zip_ref.extractall(temp_extract_dir)
                    
                    # Find the extracted ESP-IDF directory (it might be nested)
                    extracted_items = os.listdir(temp_extract_dir)
                    if len(extracted_items) == 1 and os.path.isdir(os.path.join(temp_extract_dir, extracted_items[0])):
                        # Single directory extracted, move it to final location
                        shutil.move(os.path.join(temp_extract_dir, extracted_items[0]), install_path)
                    else:
                        # Multiple items or files, move the whole temp directory
                        shutil.move(temp_extract_dir, install_path)
                finally:
                    # Clean up temporary extraction directory
                    if os.path.exists(temp_extract_dir):
                        try:
                            # On Windows, sometimes files are locked by antivirus/indexing
                            # Try multiple times with delays
                            import time
                            for attempt in range(3):
                                try:
                                    shutil.rmtree(temp_extract_dir)
                                    break
                                except (OSError, PermissionError) as e:
                                    if attempt < 2:  # Not the last attempt
                                        time.sleep(1)  # Wait 1 second
                                        continue
                                    else:
                                        # Last attempt failed, just warn and continue
                                        print(f"Warning: Could not delete temporary extraction directory {temp_extract_dir}: {e}")
                                        print("This is usually harmless - you can manually delete this directory later.")
                        except Exception as e:
                            print(f"Warning: Unexpected error during cleanup: {e}")
            
        finally:
            # Clean up temporary file
            try:
                if os.path.exists(tmp_path):
                    os.unlink(tmp_path)
            except (OSError, PermissionError) as e:
                print(f"Warning: Could not delete temporary file {tmp_path}: {e}")
            
        print(f"ESP-IDF v{version} successfully installed to {install_path}")
        
        # Verify installation
        export_script = "export.bat" if os.name == 'nt' else "export.sh"
        if not os.path.exists(os.path.join(install_path, export_script)):
            print(f"WARNING: {export_script} not found in extracted ESP-IDF")
            return None
            
        return install_path
        
    except Exception as e:
        print(f"ERROR: Failed to download ESP-IDF: {e}")
        return None

def find_esp_idf(auto_download: bool = False) -> Optional[str]:
    """Find ESP-IDF installation path"""
    
    # Check environment variable first
    if 'IDF_PATH' in os.environ:
        idf_path = os.environ['IDF_PATH']
        print(f"Found existing ESP-IDF environment: {idf_path}")
        response = input("Do you want to use this path? [y/n]: ").strip().lower()
        if response not in ['n', 'no']:
            return idf_path
    
    print("Searching for ESP-IDF installation...")
    
    # Common ESP-IDF installation paths
    search_paths = []
    
    if os.name == 'nt':  # Windows
        username = os.environ.get('USERNAME', '')
        # Get script directory for local ESP-IDF installations
        script_dir = os.path.dirname(os.path.abspath(__file__))
        search_paths = [
            r"C:\esp\esp-idf",
            rf"C:\Users\{username}\esp\esp-idf",
            r"C:\espressif\esp-idf",
            rf"C:\Users\{username}\espressif\esp-idf",
            r"C:\esp-idf",
            r"D:\esp\esp-idf",
            r"D:\espressif\esp-idf",
            r"C:\Program Files\esp-idf",
            r"C:\Program Files (x86)\esp-idf",
            r"C:\tools\esp-idf",
            r"C:\esp\esp-idf-v6.0.2",
            # r"S:\Espressif\frameworks\esp-idf-v5.5",
            r"C:\esp\esp-idf-v5.5",
            r"C:\esp\esp-idf-v5.4.1",
            os.path.join(script_dir, "esp-idf-v6.0.2"),
            os.path.join(script_dir, "esp-idf-v5.5"),
            os.path.join(script_dir, "esp-idf-v5.4.1"),
            os.path.join(script_dir, "esp-idf")
        ]
    else:  # Unix-like systems
        home = os.path.expanduser("~")
        # Get script directory for local ESP-IDF installations
        script_dir = os.path.dirname(os.path.abspath(__file__))
        search_paths = [
            f"{home}/esp/esp-idf",
            f"{home}/.espressif/esp-idf",
            "/opt/esp-idf",
            "/usr/local/esp-idf",
            "/opt/espressif/esp-idf",
            f"{home}/esp/esp-idf-v5.5",
            f"{home}/esp/esp-idf-v5.4.1",
            f"{home}/esp/esp-idf-v6.0.2",
            f"{home}/esp/v5.5/esp-idf",
            f"{home}/esp/v5.4.1/esp-idf",
            os.path.join(script_dir, "esp-idf-v6.0.2"),
            os.path.join(script_dir, "esp-idf-v5.5"),
            os.path.join(script_dir, "esp-idf-v5.4.1"),
            os.path.join(script_dir, "esp-idf")
        ]
    
    # Search for ESP-IDF in common locations
    for path in search_paths:
        export_script = "export.bat" if os.name == 'nt' else "export.sh"
        if os.path.exists(os.path.join(path, export_script)):
            print(f"Found ESP-IDF at: {path}")
            choice = input("Use this ESP-IDF path? [y/n]: ").strip().lower()
            if choice not in ['n', 'no']:
                return path
            else:
                print("skipping detected ESP-IDF path per user choice")
    
    # Search for version-specific folders
    print("Searching for version-specific installations...")
    for drive in (['C:', 'D:'] if os.name == 'nt' else ['/']):
        base_paths = []
        if os.name == 'nt':
            username = os.environ.get('USERNAME', '')
            base_paths = [
                f"{drive}\\esp",
                f"{drive}\\espressif", 
                f"{drive}\\Users\\{username}\\esp"
            ]
        else:
            base_paths = ["/opt", "/usr/local", os.path.expanduser("~/esp")]
        
        for base_path in base_paths:
            if os.path.exists(base_path):
                try:
                    for item in os.listdir(base_path):
                        if item.startswith("esp-idf-"):
                            full_path = os.path.join(base_path, item)
                            export_script = "export.bat" if os.name == 'nt' else "export.sh"
                            if os.path.exists(os.path.join(full_path, export_script)):
                                print(f"Found ESP-IDF at: {full_path}")
                                choice = input("Use this ESP-IDF path? [y/n]: ").strip().lower()
                                if choice not in ['n', 'no']:
                                    return full_path
                                else:
                                    print("skipping detected ESP-IDF path per user choice")
                except (PermissionError, OSError):
                    continue
    
    # If auto-download is enabled, offer to download ESP-IDF
    if auto_download:
        print("\nESP-IDF not found. Would you like to download it automatically?")
        print("Available versions:")
        print("  1. ESP-IDF v6.0.2 (recommended)")
        print("  2. ESP-IDF v5.5.1")
        print("  3. ESP-IDF v5.4.1")
        print("  4. Manual path input")
        print("  5. Exit")
        
        choice = input("Enter your choice (1-5): ").strip()
        
        if choice == '1':
            return download_esp_idf("6.0.2")
        elif choice == '2':
            return download_esp_idf("5.5.1")
        elif choice == '3':
            return download_esp_idf("5.4.1")
        elif choice == '4':
            pass  # Fall through to manual input
        else:
            print("Exiting build script.")
            return None
    
    # Manual input
    print("ESP-IDF not found in common locations.")
    print()
    if os.name == 'nt':
        print("Please enter the path to your ESP-IDF installation:")
        print("Example: C:\\esp\\esp-idf")
        print(f"Example: C:\\Users\\{os.environ.get('USERNAME', '')}\\esp\\esp-idf")
    else:
        print("Please enter the path to your ESP-IDF installation:")
        print("Example: ~/esp/esp-idf")
        print("Example: /opt/esp-idf")
    print()
    
    custom_path = input("ESP-IDF Path (or press Enter to exit): ").strip()
    if not custom_path:
        print("Exiting build script.")
        return None
    
    return custom_path.strip('"')

def validate_esp_idf(idf_path: str) -> bool:
    """Validate ESP-IDF installation"""
    export_script = "export.bat" if os.name == 'nt' else "export.sh"
    export_path = os.path.join(idf_path, export_script)
    
    if not os.path.exists(export_path):
        print(f"ERROR: Invalid ESP-IDF path. {export_script} not found in {idf_path}")
        print("Please ensure you have ESP-IDF v6.0.2 (recommended), v5.5.1, or v5.4.1 installed.")
        print("Download v6.0.2: https://github.com/espressif/esp-idf/releases/tag/v6.0.2")
        print("Download v5.5.1: https://github.com/espressif/esp-idf/releases/tag/v5.5.1")
        print("Download v5.4.1: https://github.com/espressif/esp-idf/releases/tag/v5.4.1")
        return False
    
    tools_path = os.path.join(idf_path, "tools")
    if not os.path.exists(tools_path):
        print("WARNING: ESP-IDF tools directory not found. Installation may be incomplete.")
    
    print("ESP-IDF validation successful!")
    return True

def setup_esp_idf_environment(idf_path: str) -> Tuple[Dict[str, str], str]:
    """Setup ESP-IDF environment and return environment variables and shell command prefix"""
    print("\nInitializing ESP-IDF environment...")
    
    # Set IDF_PATH
    env = os.environ.copy()
    env['IDF_PATH'] = idf_path
    
    if os.name == 'nt':
        # On Windows, we need to create a command prefix that sources export.bat
        export_bat = os.path.join(idf_path, "export.bat")
        
        # Test if export.bat works
        test_cmd = f'cmd /c "\"{export_bat}\" && echo ESP-IDF-READY"'
        try:
            result = subprocess.run(test_cmd, shell=True, capture_output=True, text=True, env=env, cwd=idf_path)
            if result.returncode != 0 or "ESP-IDF-READY" not in result.stdout:
                print(f"ERROR: export.bat failed to execute properly")
                print(f"stdout: {result.stdout}")
                print(f"stderr: {result.stderr}")
                return {}, ""
        except subprocess.CalledProcessError as e:
            print(f"ERROR: Failed to test ESP-IDF environment: {e}")
            return {}, ""
        
        # Create command prefix for running ESP-IDF commands
        cmd_prefix = f'cmd /c "\"{export_bat}\" >nul 2>&1 &&'
        
        print(f"ESP-IDF environment validated successfully")
        return env, cmd_prefix
        
    else:
        # On Unix-like systems, source the export.sh
        export_sh = os.path.join(idf_path, "export.sh")
        source_cmd = f'source "{export_sh}" && env'
        try:
            result = subprocess.run(['bash', '-c', source_cmd], capture_output=True, text=True, env=env)
            if result.returncode != 0:
                raise subprocess.CalledProcessError(result.returncode, source_cmd)
            
            # Parse environment variables from output
            for line in result.stdout.split('\n'):
                if '=' in line:
                    key, value = line.split('=', 1)
                    env[key] = value
                    
        except subprocess.CalledProcessError:
            print("ERROR: Failed to initialize ESP-IDF environment")
            return {}, ""
    
    print(f"Using ESP-IDF from: {idf_path}")
    return env, ""

def get_build_targets() -> List[Dict[str, str]]:
    """Get the build target configuration matrix"""
    return [
        {"name": "esp32-generic", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.default.esp32", "zip_name": "esp32-generic.zip"},
        {"name": "esp32s2-generic", "idf_target": "esp32s2", "sdkconfig_file": "configs/sdkconfig.default.esp32s2", "zip_name": "esp32s2-generic.zip"},
        {"name": "esp32s3-generic", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.default.esp32s3", "zip_name": "esp32s3-generic.zip"},
        {"name": "esp32c3-generic", "idf_target": "esp32c3", "sdkconfig_file": "configs/sdkconfig.default.esp32c3", "zip_name": "esp32c3-generic.zip"},
        {"name": "esp32c5-generic", "idf_target": "esp32c5", "sdkconfig_file": "configs/sdkconfig.default.esp32c5", "zip_name": "esp32c5-generic-v01.zip"},
        {"name": "esp32c6-generic", "idf_target": "esp32c6", "sdkconfig_file": "configs/sdkconfig.default.esp32c6", "zip_name": "esp32c6-generic.zip"},
        {"name": "CrowPanel Advanced P4 7/9/10.1-inch", "idf_target": "esp32p4", "sdkconfig_file": "configs/sdkconfig.crowpanel_advanced_p4_mipi_1024x600", "zip_name": "CrowPanel_Advanced_P4_7_9_10.1inch.zip"},
        {"name": "CrowPanel Advanced P4 7/9/10.1-inch v1.1", "idf_target": "esp32p4", "sdkconfig_file": "configs/sdkconfig.crowpanel_advanced_p4_mipi_1024x600_v11", "zip_name": "CrowPanel_Advanced_P4_7_9_10.1inch_v1.1.zip"},
        {"name": "CrowPanel Advanced P4 5-inch", "idf_target": "esp32p4", "sdkconfig_file": "configs/sdkconfig.crowpanel_advanced_p4_rgb_800x480", "zip_name": "CrowPanel_Advanced_P4_5inch.zip"},
        {"name": "Awok V5", "idf_target": "esp32s2", "sdkconfig_file": "configs/sdkconfig.default.esp32s2", "zip_name": "esp32v5_awok.zip"},
        {"name": "ghostboard", "idf_target": "esp32c6", "sdkconfig_file": "configs/sdkconfig.ghostboard", "zip_name": "ghostboard.zip"},
        {"name": "MarauderV4_FlipperHub", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.marauderv4", "zip_name": "MarauderV4_FlipperHub.zip"},
        {"name": "MarauderV6&AwokDual", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.marauderv6", "zip_name": "MarauderV6_AwokDual.zip"},
        {"name": "AwokMini", "idf_target": "esp32s2", "sdkconfig_file": "configs/sdkconfig.awokmini", "zip_name": "AwokMini.zip"},
        {"name": "ESP32-S3-Cardputer", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.cardputer", "zip_name": "ESP32-S3-Cardputer.zip"},
        {"name": "CYD2USB", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.CYD2USB", "zip_name": "CYD2USB.zip"},
        {"name": "CYDMicroUSB", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.CYDMicroUSB", "zip_name": "CYDMicroUSB.zip"},
        {"name": "CYDDualUSB", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.CYDDualUSB", "zip_name": "CYDDualUSB.zip"},
        {"name": "CYD2USB2.4_Inch", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.CYD2USB2.4Inch", "zip_name": "CYD2USB2.4Inch.zip"},
        {"name": "CYD2USB2.4_Inch_C", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.CYD2USB2.4Inch_C_Varient", "zip_name": "CYD2USB2.4Inch_C.zip"},
        {"name": "CYD2432S028R", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.CYD2432S028R", "zip_name": "CYD2432S028R.zip"},
        {"name": "Waveshare_LCD", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.waveshare7inch", "zip_name": "Waveshare_LCD.zip"},
        {"name": "Crowtech_LCD", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowtech7inch", "zip_name": "Crowtech_LCD.zip"},
        {"name": "CrowPanel 4.2-inch E-paper", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_42_epaper", "zip_name": "CrowPanel_4.2_Epaper.zip"},
        {"name": "CrowPanel 5.79-inch E-paper", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_579_epaper", "zip_name": "CrowPanel_5.79_Epaper.zip"},
        {"name": "CrowPanel Advance 2.4-inch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_advance24", "zip_name": "CrowPanel_Advance_2.4inch.zip"},
        {"name": "CrowPanel Advance 2.8-inch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_advance28", "zip_name": "CrowPanel_Advance_2.8inch.zip"},
        {"name": "CrowPanel Advance 3.5-inch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_advance35", "zip_name": "CrowPanel_Advance_3.5inch.zip"},
        {"name": "CrowPanel Advance 4.3-inch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_advance43", "zip_name": "CrowPanel_Advance_4.3inch.zip"},
        {"name": "CrowPanel Advance 5-inch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_advance5", "zip_name": "CrowPanel_Advance_5inch.zip"},
        {"name": "CrowPanel Advance 7-inch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_advance7", "zip_name": "CrowPanel_Advance_7inch.zip"},
        {"name": "Sunton_LCD", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.sunton7inch", "zip_name": "Sunton_LCD.zip"},
        {"name": "JC3248W535EN_LCD", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.JC3248W535EN", "zip_name": "JC3248W535EN_LCD.zip"},
        {"name": "Flipper_JCMK_GPS", "idf_target": "esp32s2", "sdkconfig_file": "configs/sdkconfig.flipper.jcmk_gps", "zip_name": "Flipper_JCMK_GPS.zip"},
        {"name": "T-Deck", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.tdeck", "zip_name": "LilyGo-T-Deck.zip"},
        {"name": "TEmbedC1101", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.TEmbedC1101", "zip_name": "LilyGo-TEmbedC1101.zip"},
        {"name": "S3TWatch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.S3TWatch", "zip_name": "LilyGo-S3TWatch-2020.zip"},
        {"name": "Elecrow CrowPanel 1.28-inch Rotary", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.crowpanel_1p28_rotary", "zip_name": "CrowPanel_1.28inch_Rotary.zip"},
        {"name": "TDisplayS3-Touch", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.TDisplayS3-Touch", "zip_name": "LilyGo-TDisplayS3-Touch.zip"},
        {"name": "JCMK_DevBoardPro", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.JCMK_DevBoardPro", "zip_name": "JCMK_DevBoardPro.zip"},
        {"name": "RabbitLabs_Minion", "idf_target": "esp32", "sdkconfig_file": "configs/sdkconfig.minion", "zip_name": "RabbitLabs_Minion.zip"},
        {"name": "Lolin_S3_Pro", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.lolins3pro", "zip_name": "Lolin_S3_Pro.zip"},
        {"name": "Cardputer ADV", "idf_target": "esp32s3", "sdkconfig_file": "configs/sdkconfig.cardputeradv", "zip_name": "CardputerADV.zip"},
        {"name": "Marauder V8", "idf_target": "esp32c5", "sdkconfig_file": "configs/sdkconfig.MarauderV8", "zip_name": "MarauderV8.zip"},
        {"name": "Marauder Pancake", "idf_target": "esp32c5", "sdkconfig_file": "configs/sdkconfig.Pancake", "zip_name": "MarauderPancake.zip"},
        {"name": "Banshee C5", "idf_target": "esp32c5", "sdkconfig_file": "configs/sdkconfig.somethingsomething", "zip_name": "Banshee-C5.zip"}
    ]

def validate_project_directory() -> bool:
    """Validate we're in the correct project directory"""
    print("\nChecking project directory...")
    
    if not os.path.exists("configs"):
        print("ERROR: configs directory not found. Please run this script from the Ghost ESP project root directory.")
        print(f"Current directory: {os.getcwd()}")
        return False
    
    if not os.path.exists("CMakeLists.txt"):
        print("ERROR: CMakeLists.txt not found. Please run this script from the Ghost ESP project root directory.")
        print(f"Current directory: {os.getcwd()}")
        return False
    
    print(f"Project directory verified: {os.getcwd()}")
    return True

def select_targets(targets: List[Dict[str, str]], args) -> List[int]:
    """Select which targets to build"""
    if args.targets:
        if 'all' in args.targets:
            return list(range(len(targets)))
        else:
            try:
                selected = [int(t) for t in args.targets]
                # Validate target indices
                for idx in selected:
                    if idx < 0 or idx >= len(targets):
                        print(f"ERROR: Invalid target index {idx}. Valid range: 0-{len(targets)-1}")
                        return []
                return selected
            except ValueError:
                print("ERROR: Invalid target specification. Use numbers or 'all'")
                return []
    
    # Interactive selection
    print("\nAvailable build targets:")
    for i, target in enumerate(targets):
        print(f"{i:2d}: {target['name']}")
    
    print("\nEnter target numbers to build (space-separated), or 'all' for all targets:")
    user_input = input().strip()
    
    if user_input.lower() == 'all':
        return list(range(len(targets)))
    
    try:
        selected = [int(t) for t in user_input.split()]
        # Validate target indices
        for idx in selected:
            if idx < 0 or idx >= len(targets):
                print(f"ERROR: Invalid target index {idx}. Valid range: 0-{len(targets)-1}")
                return []
        return selected
    except ValueError:
        print("ERROR: Invalid input. Please enter numbers separated by spaces.")
        return []

def run_idf_command(cmd: List[str], env: Dict[str, str], cmd_prefix: str = "", cwd: str = None) -> bool:
    """Run an idf.py command with proper environment"""
    try:
        if os.name == 'nt' and cmd_prefix:
            # On Windows, use the command prefix that sources export.bat
            full_cmd = f'{cmd_prefix} {" ".join(cmd)}"'
            result = subprocess.run(full_cmd, env=env, cwd=cwd, check=True, shell=True)
            return True
        else:
            # Direct execution for Unix or when no prefix needed
            result = subprocess.run(cmd, env=env, cwd=cwd, check=True)
            return True
            
    except subprocess.CalledProcessError as e:
        print(f"ERROR: Command failed with exit code {e.returncode}")
        return False
    except FileNotFoundError:
        print("ERROR: idf.py not found. Make sure ESP-IDF environment is properly set up.")
        return False

def _is_cmake_build_dir(path: str) -> bool:
    """best-effort check if a directory looks like a cmake build dir"""
    cmake_cache = os.path.join(path, "CMakeCache.txt")
    cmake_files = os.path.join(path, "CMakeFiles")
    ninja_file = os.path.join(path, "build.ninja")
    return os.path.exists(cmake_cache) or os.path.exists(cmake_files) or os.path.exists(ninja_file)

def _remove_non_cmake_build_dir() -> None:
    """if 'build' exists but isn't cmake, nuke it so idf.py doesn't bitch"""
    build_dir = os.path.join(os.getcwd(), "build")
    if os.path.exists(build_dir) and not _is_cmake_build_dir(build_dir):
        try:
            shutil.rmtree(build_dir)
            print("Removed non-CMake build directory: build")
        except Exception as e:
            print(f"WARNING: Failed to remove non-CMake build directory: {e}")

def _set_target_with_recovery(idf_target: str, env: Dict[str, str], cmd_prefix: str) -> bool:
    """try set-target; if it fails, remove bad build dir and retry once"""
    if run_idf_command(['idf.py', 'set-target', idf_target], env, cmd_prefix):
        return True
    print("Set-target failed once. Checking 'build' directory and retrying...")
    _remove_non_cmake_build_dir()
    return run_idf_command(['idf.py', 'set-target', idf_target], env, cmd_prefix)

def run_menuconfig(target: Dict[str, str], env: Dict[str, str], cmd_prefix: str = "") -> bool:
    """Run menuconfig for a specific target configuration"""
    print(f"\n{'='*50}")
    print(f"Running menuconfig for: {target['name']}")
    print(f"Target: {target['idf_target']}")
    print(f"Config: {target['sdkconfig_file']}")
    print(f"{'='*50}")
    
    # Apply the target's config first
    print(f"Applying base configuration from {target['sdkconfig_file']}...")
    
    # Remove existing sdkconfig files
    for config_file in ['sdkconfig', 'sdkconfig.defaults']:
        if os.path.exists(config_file):
            os.remove(config_file)
    
    # Copy the target's config as defaults
    try:
        shutil.copy2(target['sdkconfig_file'], 'sdkconfig.defaults')
    except Exception as e:
        print(f"ERROR: Failed to copy config file: {e}")
        return False
    
    # Set the target
    print(f"Setting target to {target['idf_target']}...")
    _remove_non_cmake_build_dir()
    if not _set_target_with_recovery(target['idf_target'], env, cmd_prefix):
        print("ERROR: Failed to set target")
        return False
    
    # Run menuconfig
    print("\nLaunching menuconfig...")
    print("Use the interface to modify configuration, then save and exit.")
    if not run_idf_command(['idf.py', 'menuconfig'], env, cmd_prefix):
        print("ERROR: Menuconfig failed")
        return False
    
    # Ask if user wants to save the modified config back to the original file
    print("\nMenuconfig completed.")
    save_config = input(f"Save modified configuration back to {target['sdkconfig_file']}? [y/n]: ").strip().lower()
    
    if save_config in ['y', 'yes']:
        try:
            if os.path.exists('sdkconfig'):
                shutil.copy2('sdkconfig', target['sdkconfig_file'])
                # Also copy to sdkconfig.defaults to ensure it's used in the build
                shutil.copy2('sdkconfig', 'sdkconfig.defaults')
                print(f"Configuration saved to {target['sdkconfig_file']} and applied to build")
            else:
                print("Warning: No sdkconfig file found to save")
        except Exception as e:
            print(f"ERROR: Failed to save config: {e}")
            return False
    else:
        print("Configuration changes not saved to original file.")
        # Even if not saving to original file, keep the changes in sdkconfig.defaults for this build
        if os.path.exists('sdkconfig'):
            try:
                shutil.copy2('sdkconfig', 'sdkconfig.defaults')
                print("Configuration changes will be used for this build only")
            except Exception as e:
                print(f"Warning: Failed to apply config changes to build: {e}")
    
    return True

def build_target(target: Dict[str, str], env: Dict[str, str], cmd_prefix: str = "") -> bool:
    """Build a specific target"""
    print(f"\n{'='*40}")
    print(f"Building: {target['name']}")
    print(f"Target: {target['idf_target']}")
    print(f"Config: {target['sdkconfig_file']}")
    print(f"{'='*40}")
    
    # Check if config file exists
    print(f"Checking for config file: {target['sdkconfig_file']}")
    if not os.path.exists(target['sdkconfig_file']):
        print(f"ERROR: Config file {target['sdkconfig_file']} not found!")
        print(f"Current directory: {os.getcwd()}")
        if os.path.exists("configs"):
            print("Directory contents:")
            for item in os.listdir("configs"):
                print(f"  {item}")
        return False
    
    print(f"Config file found: {target['sdkconfig_file']}")
    
    # Apply custom SDK config
    # Always copy the target's own config file.  A stale sdkconfig.defaults
    # from a previous target (e.g. P4 with BT disabled) would otherwise leak
    # into the next build and break it (missing NimBLE headers, etc.).
    print(f"Applying config from {target['sdkconfig_file']}...")
    try:
        shutil.copy2(target['sdkconfig_file'], "sdkconfig.defaults")
        shutil.copy2(target['sdkconfig_file'], "sdkconfig")
    except Exception as e:
        print(f"ERROR: Failed to copy config file: {e}")
        return False
    
    # Verify the file was created and has content
    if not os.path.exists("sdkconfig.defaults"):
        print("ERROR: sdkconfig.defaults was not created")
        return False
    
    if os.path.getsize("sdkconfig.defaults") == 0:
        print("ERROR: sdkconfig.defaults is empty")
        return False
    
    print("Config file copied successfully.")
    
    # Set target
    print(f"Setting IDF target to {target['idf_target']}...")
    _remove_non_cmake_build_dir()
    if not _set_target_with_recovery(target['idf_target'], env, cmd_prefix):
        print(f"ERROR: Failed to set target {target['idf_target']}")
        return False
    
    # Clean and build
    print("Cleaning previous build...")
    if not run_idf_command(['idf.py', 'clean'], env, cmd_prefix):
        print("ERROR: Failed to clean")
        return False
    
    print("Building project...")
    if not run_idf_command(['idf.py', 'build'], env, cmd_prefix):
        print(f"ERROR: Build failed for {target['name']}")
        return False
    
    # Verify bootloader exists
    bootloader_path = os.path.join("build", "bootloader", "bootloader.bin")
    if not os.path.exists(bootloader_path):
        print("ERROR: Bootloader not found. Build may have failed.")
        return False
    
    # Package artifacts
    print("Packaging artifacts...")
    artifact_dir = os.path.join("local_builds", target['name'])
    
    # Remove existing artifact directory
    if os.path.exists(artifact_dir):
        shutil.rmtree(artifact_dir)
    
    os.makedirs(artifact_dir, exist_ok=True)
    
    # Copy required files
    try:
        shutil.copy2(os.path.join("build", "partition_table", "partition-table.bin"), 
                     os.path.join(artifact_dir, "partitions.bin"))
        
        shutil.copy2(os.path.join("build", "bootloader", "bootloader.bin"),
                     os.path.join(artifact_dir, "bootloader.bin"))
        
        # Find and copy the main firmware binary
        build_dir = Path("build")
        firmware_found = False
        for bin_file in build_dir.glob("*.bin"):
            if bin_file.name not in ["bootloader.bin", "partition-table.bin"]:
                shutil.copy2(bin_file, os.path.join(artifact_dir, "firmware.bin"))
                firmware_found = True
                break
        
        if not firmware_found:
            print("ERROR: Failed to find firmware binary")
            return False

        if target['idf_target'] == 'esp32p4':
            shutil.copy2(os.path.join('firmware', 'crowpanel_p4', 'network_adapter.bin'),
                         os.path.join(artifact_dir, 'network_adapter.bin'))
            
    except Exception as e:
        print(f"ERROR: Failed to copy build artifacts: {e}")
        return False
    
    print(f"Contents of {artifact_dir}:")
    for item in os.listdir(artifact_dir):
        print(f"  {item}")
    
    # Create ZIP file
    zip_path = os.path.join("local_builds", target['zip_name'])
    print(f"Creating ZIP file: {zip_path}")
    
    try:
        with zipfile.ZipFile(zip_path, 'w', zipfile.ZIP_DEFLATED) as zipf:
            for item in os.listdir(artifact_dir):
                item_path = os.path.join(artifact_dir, item)
                zipf.write(item_path, item)
        
        print(f"Successfully created: {zip_path}")
        
    except Exception as e:
        print(f"ERROR: Failed to create ZIP file: {e}")
        return False

# --- Merge binaries using esptool.py merge_bin ---
    merged_bin_path = os.path.join("local_builds", target['name'] + "-merged-gesp.bin")
    bootloader_bin = os.path.join("build", "bootloader", "bootloader.bin")
    partition_bin = os.path.join("build", "partition_table", "partition-table.bin")
    firmware_bin = None
    for bin_file in build_dir.glob("*.bin"):
        if bin_file.name not in ["bootloader.bin", "partition-table.bin"]:
            firmware_bin = str(bin_file)
            break

    if firmware_bin:
        # Determine offsets (adjust if needed for your project)
        boot_offset = "0x1000" if target['idf_target'] in ["esp32", "esp32s2"] else "0x0"
        partition_offset = "0x8000"
        firmware_offset = "0x10000"
        import re
        import sys

        # Source flash mode/freq/size from the resolved build config so merge_bin
        # re-stamps the image headers with the exact values the binaries were
        # compiled with. merge_bin does NOT recompute each image's SHA-256, so a
        # mismatch here silently breaks the bootloader's per-boot image check
        # (and a too-small size makes partitions past that size unreachable).
        def _resolved_cfg(key):
            sdkconfig_h = os.path.join("build", "config", "sdkconfig.h")
            try:
                with open(sdkconfig_h, "r", encoding="utf-8") as fh:
                    m = re.search(r'^#define\s+%s\s+"([^"]*)"' % re.escape(key),
                                  fh.read(), re.MULTILINE)
                if m:
                    return m.group(1)
            except OSError:
                pass
            return None

        flash_mode = _resolved_cfg("CONFIG_ESPTOOLPY_FLASHMODE") or "dio"
        flash_freq = _resolved_cfg("CONFIG_ESPTOOLPY_FLASHFREQ") or "40m"
        flash_size = _resolved_cfg("CONFIG_ESPTOOLPY_FLASHSIZE") or "4MB"

        merge_cmd = [
            sys.executable, "-m", "esptool", 
            "--chip", target['idf_target'],
            "merge-bin",
            "-o", merged_bin_path,
            "--flash-mode", flash_mode,
            "--flash-freq", flash_freq,
            "--flash-size", flash_size,
            boot_offset, bootloader_bin,
            partition_offset, partition_bin,
            firmware_offset, firmware_bin
        ]
        if target['idf_target'] == 'esp32p4':
            merge_cmd.extend([
                "0xbe0000",
                os.path.join("firmware", "crowpanel_p4", "network_adapter.bin")
            ])
        print(f"Merging binaries with: {' '.join(merge_cmd)}")
        try:
            result = subprocess.run(merge_cmd, check=True, capture_output=True, text=True)
            print("esptool.py merge_bin output:")
            print(result.stdout)
            if os.path.exists(merged_bin_path):
                print(f"Merged binary created: {merged_bin_path}")
            else:
                print("ERROR: Merged binary was not created.")
        except subprocess.CalledProcessError as e:
            print(f"ERROR: esptool.py merge_bin failed: {e}")
            print(e.stdout)
            print(e.stderr)
            return False
    else:
        print("ERROR: Firmware binary not found for merging.")
        return
    return True

def main():
    parser = argparse.ArgumentParser(description='Cross-platform build script for Ghost ESP')
    parser.add_argument('--targets', nargs='*', help='Target indices to build (space-separated) or "all"')
    parser.add_argument('--idf-path', help='Path to ESP-IDF installation')
    parser.add_argument('--no-banner', action='store_true', help='Skip banner display')
    parser.add_argument('--no-auto-download', action='store_true', help='Disable automatic ESP-IDF download')
    parser.add_argument('--menuconfig', action='store_true', help='Run menuconfig before building to modify configuration')
    
    args = parser.parse_args()
    
    if not args.no_banner:
        print_banner()
    
    # Validate project directory
    if not validate_project_directory():
        return 1
    
    # Find and validate ESP-IDF
    idf_path = args.idf_path or find_esp_idf(auto_download=not args.no_auto_download)
    if not idf_path:
        return 1
    
    if not validate_esp_idf(idf_path):
        return 1
    
    # Setup ESP-IDF environment
    env, cmd_prefix = setup_esp_idf_environment(idf_path)
    if not env:
        return 1
    
    # Get build targets
    targets = get_build_targets()
    
    # Create output directory
    os.makedirs("local_builds", exist_ok=True)
    
    # Select targets to build
    selected_targets = select_targets(targets, args)
    if not selected_targets:
        return 1
    
    # Prompt user for menuconfig if not explicitly requested
    run_menuconfig_mode = args.menuconfig
    if not run_menuconfig_mode and selected_targets:
        print("\nWould you like to run menuconfig to configure the build settings before building?")
        print("This allows you to modify configuration options for each target.")
        user_input = input("Run menuconfig? (y/n): ").strip().lower()
        run_menuconfig_mode = user_input in ['y', 'yes']
    
    if run_menuconfig_mode:
        print("\nMenuconfig mode enabled. You will configure each selected target before building.")
        for target_idx in selected_targets:
            target = targets[target_idx]
            print(f"\nConfiguring target {target_idx + 1} of {len(selected_targets)}: {target['name']}")
            if not run_menuconfig(target, env, cmd_prefix):
                print(f"ERROR: Menuconfig failed for {target['name']}")
                return 1
        
        print("\nAll targets configured. Proceeding with build...")
    
    # Build each selected target
    successful_builds = 0
    failed_builds = 0
    
    for target_idx in selected_targets:
        target = targets[target_idx]
        if build_target(target, env, cmd_prefix):
            successful_builds += 1
        else:
            failed_builds += 1
    
    # Summary
    print(f"\n{'='*40}")
    print("Build process completed!")
    print(f"Successful builds: {successful_builds}")
    print(f"Failed builds: {failed_builds}")
    print("Check the local_builds directory for output files.")
    print(f"{'='*40}")
    
    return 0 if failed_builds == 0 else 1

if __name__ == "__main__":
    sys.exit(main())
