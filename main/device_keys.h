/* Device Keys and CSR Header
 *
 * This file contains the device's private key and CSR that were generated
 * in Stage 1 of the provisioning flow.
 *
 * IMPORTANT: Replace the placeholder values below with your actual generated keys.
 * 
 * To generate these values:
 * 1. Run your key generation script (e.g., generate-device-keys.ps1)
 * 2. Copy the private key and CSR strings here
 * 3. Ensure proper formatting with \n for line breaks
 */

#ifndef DEVICE_KEYS_H
#define DEVICE_KEYS_H

// Device identifier (must match the one used in key generation)
#define DEVICE_ID "device_0070"

// Private Key (PEM format) - Generated in Stage 1
// TODO: Replace with your actual private key
#define DEVICE_PRIVATE_KEY_PEM \
    "-----BEGIN PRIVATE KEY-----\n" \
    "MIIEvQIBADANBgkqhkiG9w0BAQEFAASCBKcwggSjAgEAAoIBAQC...\n" \
    "(Replace with actual private key)\n" \
    "-----END PRIVATE KEY-----\n"

// Certificate Signing Request (CSR) - Generated in Stage 1
// TODO: Replace with your actual CSR
#define DEVICE_CSR_PEM \
    "-----BEGIN CERTIFICATE REQUEST-----\n" \
    "MIICVjCCAT4CAQAwFjEUMBIGA1UEAwwLZGV2aWNlXzAwNzAwggEiMA0GCSqG...\n" \
    "(Replace with actual CSR)\n" \
    "-----END CERTIFICATE REQUEST-----\n"

#endif // DEVICE_KEYS_H

