from cryptography.hazmat.primitives.asymmetric.ed25519 import Ed25519PrivateKey
from cryptography.hazmat.primitives import serialization

# Генерируем новую пару ключей Ed25519
private_key = Ed25519PrivateKey.generate()
public_key = private_key.public_key()

# Приватный ключ - PEM формат, храни это ОТДЕЛЬНО и БЕЗОПАСНО.
# Никогда не коммить в git, не публикуй, не отправляй никуда.
private_pem = private_key.private_bytes(
    encoding=serialization.Encoding.PEM,
    format=serialization.PrivateFormat.PKCS8,
    encryption_algorithm=serialization.NoEncryption()
)
with open("k85os_ota_private_key.pem", "wb") as f:
    f.write(private_pem)

# Публичный ключ - 32 сырых байта, это то, что зашивается в прошивку
public_raw = public_key.public_bytes(
    encoding=serialization.Encoding.Raw,
    format=serialization.PublicFormat.Raw
)

# Печатаем как C-массив для вставки в исходники прошивки
hex_bytes = ", ".join(f"0x{b:02x}" for b in public_raw)
print("Приватный ключ сохранён в k85os_ota_private_key.pem — ХРАНИ ОТДЕЛЬНО, НЕ В GIT!")
print()
print("Публичный ключ (вставить в прошивку):")
print(f"static const uint8_t K85_OTA_PUBLIC_KEY[32] = {{ {hex_bytes} }};")