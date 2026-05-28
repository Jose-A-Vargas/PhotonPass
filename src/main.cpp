#include <Arduino.h>
#include <LittleFS.h>
#include "WiFi.h"
#include "USB.h"
#include "USBHIDKeyboard.h"

// Instantiate the HID Keyboard
USBHIDKeyboard Keyboard;

// Define storage file name
const char* VAULT_FILE = "/vault.bin";

// Disable wireless entirely for absolute air-gapping
void lockDownRadios() {
    WiFi.mode(WIFI_OFF);
    btStop();
    Serial.println("[SYSTEM] Wi-Fi and Bluetooth permanently severed.");
}

// Write encrypted binary payload to flash storage
void saveEncryptedVault(const uint8_t* data, size_t dataSize) {
    File file = LittleFS.open(VAULT_FILE, FILE_WRITE);
    if (!file) {
        Serial.println("[ERROR] Failed to open vault file for writing.");
        return;
    }
    
    size_t written = file.write(data, dataSize);
    file.close();
    
    if (written == dataSize) {
        Serial.printf("[STORAGE] Successfully committed %d bytes to vault.bin\n", written);
    } else {
        Serial.println("[ERROR] Mismatch in written vault data size.");
    }
}

// Read raw encrypted bytes back from storage
void readEncryptedVault() {
    if (!LittleFS.exists(VAULT_FILE)) {
        Serial.println("[STORAGE] No vault file detected yet.");
        return;
    }

    File file = LittleFS.open(VAULT_FILE, FILE_READ);
    if (!file) {
        Serial.println("[ERROR] Failed to read vault file.");
        return;
    }

    Serial.print("[STORAGE] Current raw bytes inside vault.bin: ");
    while (file.available()) {
        Serial.printf("%02X ", file.read());
    }
    Serial.println();
    file.close();
}

void setup() {
    Serial.begin(115200);
    
    // Initialize native USB keyboard device
    Keyboard.begin();
    USB.begin();

    delay(3000); // Give user a moment to open serial monitor
    
    Serial.println("\n--- INITIALIZING HARDWARE VAULT BACKBONE ---");
    lockDownRadios();

    // Initialize LittleFS File System
    if (!LittleFS.begin(true)) { // 'true' format file system if it fails
        Serial.println("[ERROR] LittleFS Mount Failed.");
        return;
    }
    Serial.println("[STORAGE] LittleFS mounted successfully.");

    // Simulate dummy encrypted string data: "ENCRYPTED_DATA_STREAM_XYZ"
    uint8_t dummyEncryptedBlob[] = { 
        0x45, 0x4E, 0x43, 0x52, 0x59, 0x50, 0x54, 0x45, 
        0x44, 0x5F, 0x44, 0x41, 0x54, 0x41, 0x5F, 0x58, 0x59, 0x5A 
    };
    
    // Save it and read it back to confirm Flash Storage works flawlessly
    saveEncryptedVault(dummyEncryptedBlob, sizeof(dummyEncryptedBlob));
    readEncryptedVault();

    Serial.println("\n[READY] Press 'h' in your serial console to test physical USB HID typing.");
}

void loop() {
    // Check if user sends 'h' over serial monitor to execute a mock keyboard push
    if (Serial.available() > 0) {
        char incomingByte = Serial.read();
        if (incomingByte == 'h') {
            Serial.println("[HID] Simulating password injection sequence via native USB...");
            
            // Focus on a text field on your desktop before pressing this!
            Keyboard.print("SuperSecretDecryptedPassword123!");
            Keyboard.write(KEY_RETURN); // Emulate hit 'Enter' key
            
            Serial.println("[HID] Password payload safely typed.");
        }
    }
}