/* Certificate Manager Header - Arduino Version
 *
 * Handles CSR submission to backend and certificate storage/retrieval.
 */

#ifndef CERTIFICATE_MANAGER_H
#define CERTIFICATE_MANAGER_H

#include <Arduino.h>
#include <Preferences.h>

class CertificateManager {
public:
    /**
     * @brief Submit CSR to backend and retrieve signed certificates
     * 
     * @param deviceId Device identifier
     * @param token Provisioning token for authentication
     * @param backendUrl Backend server URL
     * @return 0 on success, -1 on error
     */
    static int submitCSR(const char* deviceId, const char* token, const char* backendUrl);
    
    /**
     * @brief Check if certificates are stored in Preferences
     * 
     * @return true if both device certificate and CA certificate exist
     */
    static bool hasCertificates();
    
    /**
     * @brief Load device certificate from Preferences
     * 
     * @param certBuffer Output buffer for certificate (PEM format)
     * @param bufferSize Size of the buffer
     * @return 0 on success, -1 on error
     */
    static int loadDeviceCert(char* certBuffer, size_t bufferSize);
    
    /**
     * @brief Load CA certificate from Preferences
     * 
     * @param certBuffer Output buffer for CA certificate (PEM format)
     * @param bufferSize Size of the buffer
     * @return 0 on success, -1 on error
     */
    static int loadCACert(char* certBuffer, size_t bufferSize);
    
    /**
     * @brief Get device private key (from DeviceKeys.h)
     * 
     * @return Pointer to private key string (PEM format)
     */
    static const char* getPrivateKey();

private:
    static int saveCertificate(const char* key, const char* certPem);
    static int generateCSR(char* csrBuffer, size_t bufferSize);
};

#endif // CERTIFICATE_MANAGER_H
