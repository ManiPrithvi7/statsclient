Device mTLS certificates for firmware embedding (CONFIG_USE_EMBEDDED_MTLS_CERTS).

Client cert/key are embedded from repo-root certs/ (see main/CMakeLists.txt).
Update certs/client.crt and certs/client.key there; set CONFIG_MTLS_CLIENT_DEVICE_ID
to match the certificate CN (e.g. DEVICE-15).

Root CA: ca_root.pem in this directory (PEM only). Same PROOF-CA as certs/root_certificate.txt.

primary/ is a legacy copy — build uses ../certs/ directly.
