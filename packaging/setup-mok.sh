#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
#
# Generate and enroll a project-specific MOK (Machine Owner Key) for the
# msa1-ec module.  Run ONCE; reboot to complete enrollment via MOK Manager.
#
# Does NOT touch the existing Ubuntu/DKMS MOK (if any).

set -euo pipefail

MOK_DIR=/var/lib/shim-signed/mok
KEY_PRIV="$MOK_DIR/msa1.priv"
KEY_DER="$MOK_DIR/msa1.der"
KEY_CN="msa1-ec module signing key"

if [[ $EUID -ne 0 ]]; then
    echo "Run as root: sudo $0" >&2
    exit 1
fi

echo "=== MS-A1 EC module MOK key setup ==="
echo
echo "This will:"
echo "  1. Generate an RSA-2048 signing keypair at:"
echo "       private: $KEY_PRIV"
echo "       public : $KEY_DER"
echo "  2. Request its enrollment for UEFI Secure Boot via mokutil"
echo "  3. Prompt you for a one-time enrollment password (used at next boot)"
echo
echo "After this script you MUST reboot.  At boot you will see a blue"
echo "'MOK Manager' screen.  Choose:  Enroll MOK -> Continue -> enter password"
echo

mkdir -p "$MOK_DIR"

if [[ -f $KEY_PRIV || -f $KEY_DER ]]; then
    echo "WARNING: a key already exists:"
    [[ -f $KEY_PRIV ]] && echo "  $KEY_PRIV"
    [[ -f $KEY_DER  ]] && echo "  $KEY_DER"
    read -rp "Overwrite? [y/N] " yn
    case "${yn,,}" in
        y|yes) ;;
        *) echo "Keeping existing key."; exit 0 ;;
    esac
fi

echo
echo "Generating RSA-2048 keypair..."
openssl req -new -x509 -newkey rsa:2048 -nodes -days 36500 \
    -keyout "$KEY_PRIV" -outform DER -out "$KEY_DER" \
    -subj "/CN=$KEY_CN/" \
    >/dev/null 2>&1
chmod 600 "$KEY_PRIV"
chmod 644 "$KEY_DER"
echo "  ok"
echo

echo "Requesting MOK enrollment..."
mokutil --import "$KEY_DER"
echo

cat <<EOF
=== NEXT STEPS ==============================================================

1. REBOOT NOW.

2. At the blue MOK Manager screen:
     - 'Enroll MOK'
     - 'View key 0'  -> verify subject contains:
         '$KEY_CN'
     - 'Continue'
     - 'Yes'
     - Enter the password you just set.
     - 'Reboot'

3. After reboot, verify enrollment:
     mokutil --list-enrolled | grep -A1 'msa1-ec'

4. Then install the module:
     sudo make dkms-install

============================================================================
EOF
