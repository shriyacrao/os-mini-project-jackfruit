#!/usr/bin/env bash
# environment-check.sh — Preflight check for OS-Jackfruit project
set -euo pipefail

PASS=0; WARN=0; FAIL=0

ok()   { echo "  [OK]   $*"; ((PASS++));  }
warn() { echo "  [WARN] $*"; ((WARN++));  }
fail() { echo "  [FAIL] $*"; ((FAIL++));  }

echo "========================================="
echo " OS-Jackfruit Environment Check"
echo "========================================="

# ---- OS ----
echo ""
echo "--- Operating System ---"
if [ -f /etc/os-release ]; then
    . /etc/os-release
    echo "  Distro: $PRETTY_NAME"
    if [[ "$ID" == "ubuntu" && ("$VERSION_ID" == "22.04" || "$VERSION_ID" == "24.04") ]]; then
        ok "Ubuntu $VERSION_ID detected"
    else
        warn "Expected Ubuntu 22.04 or 24.04; got $PRETTY_NAME"
    fi
else
    warn "Cannot determine OS version"
fi

# ---- Running as root ----
echo ""
echo "--- Permissions ---"
if [ "$(id -u)" -eq 0 ]; then
    ok "Running as root"
else
    fail "Must run as root (sudo ./environment-check.sh)"
fi

# ---- Kernel version ----
echo ""
echo "--- Kernel ---"
KVER=$(uname -r)
echo "  Kernel: $KVER"
KVER_MAJOR=$(echo "$KVER" | cut -d. -f1)
KVER_MINOR=$(echo "$KVER" | cut -d. -f2)
if [ "$KVER_MAJOR" -ge 5 ]; then
    ok "Kernel $KVER is supported"
else
    fail "Kernel $KVER too old (need >= 5.x)"
fi

# ---- linux-headers ----
echo ""
echo "--- Kernel Headers ---"
HDIR="/lib/modules/$KVER/build"
if [ -d "$HDIR" ]; then
    ok "Kernel headers found at $HDIR"
else
    fail "Kernel headers missing: sudo apt install linux-headers-$(uname -r)"
fi

# ---- Build tools ----
echo ""
echo "--- Build Tools ---"
for tool in gcc make ld; do
    if command -v $tool &>/dev/null; then
        ok "$tool found ($(command -v $tool))"
    else
        fail "$tool not found: sudo apt install build-essential"
    fi
done

# ---- Namespace support ----
echo ""
echo "--- Namespace Support ---"
for ns in pid uts mnt; do
    if ls /proc/self/ns/$ns &>/dev/null; then
        ok "$ns namespace available"
    else
        fail "$ns namespace not available (kernel config issue?)"
    fi
done

# ---- Secure Boot ----
echo ""
echo "--- Secure Boot ---"
if command -v mokutil &>/dev/null; then
    SB=$(mokutil --sb-state 2>/dev/null || echo "unknown")
    if echo "$SB" | grep -qi "disabled"; then
        ok "Secure Boot is disabled"
    else
        fail "Secure Boot may be enabled: $SB (kernel modules won't load without signing)"
    fi
else
    warn "mokutil not found — cannot check Secure Boot; ensure it's disabled in VM firmware"
fi

# ---- Summary ----
echo ""
echo "========================================="
echo " Results: $PASS OK  $WARN WARN  $FAIL FAIL"
echo "========================================="
if [ "$FAIL" -gt 0 ]; then
    echo " Fix the FAIL items before building."
    exit 1
else
    echo " Environment looks good — you can build!"
    exit 0
fi
