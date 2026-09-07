#!/usr/bin/env bash

set -euo pipefail
umask 077

usage() {
    cat <<'EOF'
Usage:
  generate-tls-cert.sh --output-dir DIR (--dns NAME | --ip ADDRESS)... [--renew]

Creates a private CA and a TLS server certificate. On renewal, the existing CA
is reused so deployed clients can continue trusting newly issued certificates.

Options:
  --output-dir DIR  Directory for CA and server certificate files
  --dns NAME        Add a DNS subject alternative name (repeatable)
  --ip ADDRESS      Add an IP subject alternative name (repeatable)
  --renew           Replace only the existing server key and certificate
  --help            Show this help
EOF
}

die() {
    printf 'error: %s\n' "$*" >&2
    exit 1
}

output_dir=
renew=0
declare -a dns_names=()
declare -a ip_addresses=()

while (($# > 0)); do
    case "$1" in
        --output-dir)
            (($# >= 2)) || die "--output-dir requires a value"
            output_dir=$2
            shift 2
            ;;
        --dns)
            (($# >= 2)) || die "--dns requires a value"
            dns_names+=("$2")
            shift 2
            ;;
        --ip)
            (($# >= 2)) || die "--ip requires a value"
            ip_addresses+=("$2")
            shift 2
            ;;
        --renew)
            renew=1
            shift
            ;;
        --help|-h)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

[[ -n "$output_dir" ]] || die "--output-dir is required"
(( ${#dns_names[@]} + ${#ip_addresses[@]} > 0 )) || \
    die "at least one --dns or --ip is required"
command -v openssl >/dev/null 2>&1 || die "openssl is required"

for name in "${dns_names[@]}"; do
    [[ "$name" =~ ^[A-Za-z0-9]([A-Za-z0-9.-]*[A-Za-z0-9])?$ ]] || \
        die "invalid DNS name: $name"
    [[ "$name" != *..* ]] || die "invalid DNS name: $name"
    ((${#name} <= 253)) || die "DNS name is too long: $name"
    IFS='.' read -r -a labels <<<"$name"
    for label in "${labels[@]}"; do
        ((${#label} <= 63)) || die "DNS label is too long in: $name"
        [[ "$label" =~ ^[A-Za-z0-9]([A-Za-z0-9-]*[A-Za-z0-9])?$ ]] || \
            die "invalid DNS name: $name"
    done
done
for address in "${ip_addresses[@]}"; do
    [[ "$address" =~ ^[0-9A-Fa-f:.]+$ ]] || die "invalid IP address: $address"
done

mkdir -p -- "$output_dir"
chmod 700 "$output_dir"
output_dir=$(cd -- "$output_dir" && pwd -P)

ca_key="$output_dir/ca.key"
ca_cert="$output_dir/ca.crt"
server_key="$output_dir/server.key"
server_cert="$output_dir/server.crt"
server_chain="$output_dir/server-chain.crt"

if ((renew)); then
    [[ -f "$ca_key" && -f "$ca_cert" ]] || \
        die "--renew requires existing ca.key and ca.crt in the output directory"
else
    for path in "$ca_key" "$ca_cert" "$server_key" "$server_cert" "$server_chain"; do
        [[ ! -e "$path" ]] || die "$path already exists; use --renew to rotate the server certificate"
    done
fi

tmp_dir=$(mktemp -d "$output_dir/.generate.XXXXXX")
trap 'rm -rf -- "$tmp_dir"' EXIT

if ((!renew)); then
    openssl genpkey -algorithm RSA \
        -pkeyopt rsa_keygen_bits:3072 \
        -out "$tmp_dir/ca.key" >/dev/null 2>&1
    openssl req -x509 -new -sha256 -days 3650 \
        -key "$tmp_dir/ca.key" \
        -out "$tmp_dir/ca.crt" \
        -subj "/CN=CIM Private Root CA" \
        -addext "basicConstraints=critical,CA:TRUE,pathlen:0" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -addext "subjectKeyIdentifier=hash"
    mv "$tmp_dir/ca.key" "$ca_key"
    mv "$tmp_dir/ca.crt" "$ca_cert"
fi

openssl genpkey -algorithm RSA \
    -pkeyopt rsa_keygen_bits:3072 \
    -out "$tmp_dir/server.key" >/dev/null 2>&1

common_name=${dns_names[0]:-${ip_addresses[0]}}
{
    printf '%s\n' \
        '[req]' \
        'prompt = no' \
        'distinguished_name = subject' \
        'req_extensions = request_extensions' \
        '[subject]'
    printf 'CN = %s\n' "$common_name"
    printf '%s\n' \
        '[request_extensions]' \
        'subjectAltName = @alternative_names' \
        '[server_extensions]' \
        'basicConstraints = critical,CA:FALSE' \
        'keyUsage = critical,digitalSignature,keyEncipherment' \
        'extendedKeyUsage = serverAuth' \
        'subjectKeyIdentifier = hash' \
        'authorityKeyIdentifier = keyid,issuer' \
        'subjectAltName = @alternative_names' \
        '[alternative_names]'
    dns_index=1
    for name in "${dns_names[@]}"; do
        printf 'DNS.%d = %s\n' "$dns_index" "$name"
        ((dns_index += 1))
    done
    ip_index=1
    for address in "${ip_addresses[@]}"; do
        printf 'IP.%d = %s\n' "$ip_index" "$address"
        ((ip_index += 1))
    done
} >"$tmp_dir/server.cnf"

openssl req -new -sha256 \
    -key "$tmp_dir/server.key" \
    -out "$tmp_dir/server.csr" \
    -config "$tmp_dir/server.cnf"

serial=$(openssl rand -hex 16)
openssl x509 -req -sha256 -days 825 \
    -in "$tmp_dir/server.csr" \
    -CA "$ca_cert" \
    -CAkey "$ca_key" \
    -set_serial "0x$serial" \
    -extfile "$tmp_dir/server.cnf" \
    -extensions server_extensions \
    -out "$tmp_dir/server.crt"

openssl verify -CAfile "$ca_cert" "$tmp_dir/server.crt" >/dev/null
for name in "${dns_names[@]}"; do
    openssl x509 -in "$tmp_dir/server.crt" -noout -checkhost "$name" >/dev/null
done
for address in "${ip_addresses[@]}"; do
    openssl x509 -in "$tmp_dir/server.crt" -noout -checkip "$address" >/dev/null
done

cert_public_key=$(openssl x509 -in "$tmp_dir/server.crt" -pubkey -noout | openssl sha256)
private_public_key=$(openssl pkey -in "$tmp_dir/server.key" -pubout | openssl sha256)
[[ "$cert_public_key" == "$private_public_key" ]] || die "generated certificate and key do not match"

cat "$tmp_dir/server.crt" "$ca_cert" >"$tmp_dir/server-chain.crt"
mv "$tmp_dir/server.key" "$server_key"
mv "$tmp_dir/server.crt" "$server_cert"
mv "$tmp_dir/server-chain.crt" "$server_chain"
chmod 600 "$ca_key" "$server_key"
chmod 644 "$ca_cert" "$server_cert" "$server_chain"

printf 'Generated TLS files in %s\n' "$output_dir"
printf '  Client trust certificate: %s\n' "$ca_cert"
printf '  Server certificate chain: %s\n' "$server_chain"
printf '  Server private key:       %s\n' "$server_key"
