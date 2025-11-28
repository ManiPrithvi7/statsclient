/* Device Keys and CSR Header
 *
 * This file contains the device's private key and CSR.
 * 
 * IMPORTANT: In production, these should be generated ON-DEVICE using mbedTLS
 * to ensure the private key NEVER leaves the hardware.
 * 
 * For testing purposes, we use pre-generated keys here.
 */

#ifndef DEVICE_KEYS_H
#define DEVICE_KEYS_H

// Device identifier
#define DEVICE_ID "device_0070"

// Private Key (PEM format) - RSA-2048
// WARNING: For testing only. In production, generate on-device!
#define DEVICE_PRIVATE_KEY_PEM \
    "-----BEGIN PRIVATE KEY-----\n" \
    "MIIEvwIBADANBgkqhkiG9w0BAQEFAASCBKkwggSlAgEAAoIBAQCvai7foAdfiX6W\n" \
    "hZ3l0BX1o78NeTKs5No+c9uAgH8tZHUAXYQX/NPVeJJhROmmFz73KUIF8frUjbY/\n" \
    "2ZL8K9ifwd/mt9B9JtpsBmkEYlwmUGlYJR1LNbd+MmzRKvEmSxQsPIoUtcqQe+oD\n" \
    "vAlLkoTqXfJeYSzvoRY/TlGoOAaQfEYDiLn4Qplzvof/8/vgTLf+cFETToIpt/vr\n" \
    "n+e/XdbLiO//AHGNJzXOGp4gWcToMG+MkBYukhT8iWemMBRfPcGHVKarpXHp5Nkr\n" \
    "TKHrsq1IatXZ+VY5XxCStSvEA2wHKlVnHChTbIM4LwHpPQ+P+zCpWTk9mPuFLMMM\n" \
    "aCBdw0U7AgMBAAECggEABiAWl+9+ImtqNLZgv6QnCCdaJg3L47awGkswaInUJxEh\n" \
    "dsxNLwtAmG0361adNBQOulHCAPQktqRmL0+ZLt4XF+kMuQFFMgGX0frdUu5eWmYn\n" \
    "b5EIN1aeXDVFkH5H4nbvsuXASp3Yf3gcQVohFvb8VjTGoF4TVCDTZo2FE3M1Y3ks\n" \
    "jEkQf5LM3vFmMx2pI8xauc1QmK0JpVIPDffQ1SAmJCDaDKPtCHqK/iDAkV0VnOmX\n" \
    "ScdEjWO6e4Esv11uFb7Eb8qf6F0GwicfhOQ+QAeyqX50b6qqvv2Jb7mCeTOAqgWx\n" \
    "NRb6G1jM+xHARogdILBdW9nsyz/XDXqQOTTdfYv9yQKBgQDwJ2ia2mpfhEnURYgs\n" \
    "+mFNgmXThbvGZqYYW9ZjApqMlPajXQG01fOCtZR2S06jayJPQlhsexGm3fNqwo1T\n" \
    "PtG8GLDdNOc/c7v41HyTo3fj5SmD4ssf3qKlRPx4iNY0DMXwwqX2pRLlHBzfFpdP\n" \
    "ougLd7kZMdVOFF+bPFNc7BqxSQKBgQC6/Tl7nMbIJI+W3Z4slqoyXg6Vt3N8zHGL\n" \
    "24iMS3JwRY/f2KSq4lEfePqjw6GzsdT/TgO4ngxiy5DCC2IoowoQcbWNDw1+rTW4\n" \
    "ropZT+XqpUwtw5mOJlontWzVmwvl+rAZYfAozr60qiruYhGuKW0UDfcVO0qbc2rO\n" \
    "AxS9zgEGYwKBgQCuk2dGUopLPyZQSe9xCt4a8zTEbA4RbuNFB9W0CduBYHReUyj/\n" \
    "ZRxso0T2LU5QG3xIc6lFyr0NOYFO1XjYz+y1OJmxZFjKVn6JpyWcSZPItfjU59PT\n" \
    "Kgu/6oNBt+9GzRZDK6xrvJoctLAEOC7sdDcMxw5mU0SFSugpN9Q902CgCQKBgQC6\n" \
    "5pToS2Iddv3XDBkn9EiWI5FscHuMyETOSFaJ9HekZMNUORUOgTwYuzG7MrWUCTIf\n" \
    "JfluNPuoXFSKwBoTCDPtD9sp7VvNvI+v0zYR41yqVyJ3s8TuYsNGYX8xWtJfw1z1\n" \
    "YgFMqKnRpy1WLMwDSwDuRK8tl6ARFSIyXL4Eob5AhQKBgQCJhnO6ZE03zVwhSjp2\n" \
    "Ec/n1dx786w2cak7LZLTfdGyzaeiVRxDej2o67AAJ4CIyCakCy+nVke3at+sZOF9\n" \
    "7L312Pj3EjXp0YqLooBnHNRf4lSicAbolJYzN+JhAgG6PqX3uIw+GvfWYGvh8mUQ\n" \
    "uow0Mc5NHdZxSUxVXtlMaESnkQ==\n" \
    "-----END PRIVATE KEY-----\n"

// Certificate Signing Request (CSR) - for device_0070
// This will be submitted to the backend for signing
#define DEVICE_CSR_PEM \
    "-----BEGIN CERTIFICATE REQUEST-----\n" \
    "MIICWzCCAUMCAQAwFjEUMBIGA1UEAwwLZGV2aWNlXzAwNzAwggEiMA0GCSqGSIb3\n" \
    "DQEBAQUAA4IBDwAwggEKAoIBAQCvai7foAdfiX6WhZ3l0BX1o78NeTKs5No+c9uA\n" \
    "gH8tZHUAXYQX/NPVeJJhROmmFz73KUIF8frUjbY/2ZL8K9ifwd/mt9B9JtpsBmkE\n" \
    "YlwmUGlYJR1LNbd+MmzRKvEmSxQsPIoUtcqQe+oDvAlLkoTqXfJeYSzvoRY/TlGo\n" \
    "OAaQfEYDiLn4Qplzvof/8/vgTLf+cFETToIpt/vrn+e/XdbLiO//AHGNJzXOGp4g\n" \
    "WcToMG+MkBYukhT8iWemMBRfPcGHVKarpXHp5NkrTKHrsq1IatXZ+VY5XxCStSvE\n" \
    "A2wHKlVnHChTbIM4LwHpPQ+P+zCpWTk9mPuFLMMMaCBdw0U7AgMBAAGgADANBgkq\n" \
    "hkiG9w0BAQsFAAOCAQEAGbE0ErYkwV8kQl9DjAcwqXOJFRm33yy3mRuToeqKzz19\n" \
    "E8WKCh/Wh0Y9XcxlQEqFs2l5Akt8vOwsiFhimxID9iLP9ZzIw37fqHwdnvQ+7Tti\n" \
    "LbBARf3rMsCKk2jmzt5xOZ+Uw82KtltwN156O8DYnd/dOvbLpfmV3A2qcJPO0UTe\n" \
    "5CHycQnU8P6e62CITMkZsxrOdSGIMHw7O3kGJt3HY1daglfuL/Kh57bd7PYswvmt\n" \
    "s1dRYlauOQ9ihbfYVt8Q7/zNMfu3VaX2xDsDYx+z+p67z1AxTYD7/Rd8qpACkupK\n" \
    "YxpRKDOk2M5F3dPYXt/nAw7rxpFvVO4jGjnV8nPOpg==\n" \
    "-----END CERTIFICATE REQUEST-----\n"

#endif // DEVICE_KEYS_H
