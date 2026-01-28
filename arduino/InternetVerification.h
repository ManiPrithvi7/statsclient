/* Internet Verification Header - Arduino Version
 */

#ifndef INTERNET_VERIFICATION_H
#define INTERNET_VERIFICATION_H

#include <Arduino.h>

class InternetVerification {
public:
    /**
     * @brief Verify internet connectivity by accessing test endpoint
     * 
     * Makes an HTTPS GET request to verify the device has internet access.
     * 
     * @return 0 if internet access is confirmed, -1 otherwise
     */
    static int test();
};

#endif // INTERNET_VERIFICATION_H
