Device mTLS certificates for firmware embedding (CONFIG_USE_EMBEDDED_MTLS_CERTS).

Copy from the Node reference client before building:

  cp ~/Desktop/mtls_client/mtlsclient/src/certs/primary/client.crt primary/
  cp ~/Desktop/mtls_client/mtlsclient/src/certs/primary/client.key primary/

Root CA: use ca_root.pem (PEM only, no banner lines) or copy from
mtlsclient/src/certs/root_certifacite.txt and strip non-PEM lines.

These files are gitignored — do not commit device private keys.
