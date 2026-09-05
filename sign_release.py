import sys
import hashlib
from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

if len(sys.argv) != 2:
    print("Usage: python sign_release.py <path_to_bin>")
    sys.exit(1)

bin_path = sys.argv[1]

with open("k85os_ota_private_key.pem", "rb") as f:
    private_key = serialization.load_pem_private_key(f.read(), password=None)

with open(bin_path, "rb") as f:
    data = f.read()

sha256_hash = hashlib.sha256(data).digest()
signature = private_key.sign(sha256_hash)
signature_hex = signature.hex()

sig_path = bin_path + ".sig"
with open(sig_path, "w") as f:
    f.write(signature_hex)

print(f"Signed {bin_path}")
print(f"Signature written to {sig_path}")
print(f"Signature: {signature_hex}")