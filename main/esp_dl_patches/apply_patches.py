#!/usr/bin/env python3
"""
Apply ESP-DL patches for the dooirobot board.

Why this exists
---------------
On ESP32-P4, ESP-DL forces the SIMD image transform path
(CONFIG_PIE_V2_BOOST == 1 in dl_define_private.hpp). The P4 SIMD RGB888
resize kernel (dl_esp32p4_resize_nn_*.S) can raise a "Load access fault"
while a model input image is being resized. The crash signature is identical
to the still-open upstream esp-dl issue #305
(https://github.com/espressif/esp-dl/issues/305).

The workaround forces the generic scalar transform path, which performs the
same, bounded, per-pixel reads and works on every other supported target.
Model inference kernels are NOT touched.

Because the IDF component manager verifies a managed component against its
CHECKSUMS.json / .component_hash and re-downloads it when a file was modified,
this script also updates both after patching, exactly like
main/wifi_connect_patches/apply_patches.py does for 78__esp-wifi-connect.

Usage:
    python apply_patches.py <project_dir>

The script is idempotent - it checks for a marker before applying each patch.
"""

import hashlib
import json
import os
import sys


def compute_sha256(filepath):
    """Compute SHA256 hash of a file."""
    sha256 = hashlib.sha256()
    with open(filepath, 'rb') as f:
        for chunk in iter(lambda: f.read(65536), b''):
            sha256.update(chunk)
    return sha256.hexdigest()


def update_checksums(component_dir, patches):
    """
    Update CHECKSUMS.json and .component_hash after patches are applied.
    """
    checksums_path = os.path.join(component_dir, 'CHECKSUMS.json')
    component_hash_path = os.path.join(component_dir, '.component_hash')

    # Read existing checksums
    with open(checksums_path, 'r') as f:
        checksums = json.load(f)

    # Update checksums for modified files
    modified_files = set(p['file'] for p in patches)
    updated = False
    for entry in checksums['files']:
        if entry['path'] in modified_files:
            filepath = os.path.join(component_dir, entry['path'])
            if os.path.exists(filepath):
                new_hash = compute_sha256(filepath)
                new_size = os.path.getsize(filepath)
                if entry['hash'] != new_hash:
                    print(f"  Updated checksum: {entry['path']}")
                    entry['hash'] = new_hash
                    entry['size'] = new_size
                    updated = True

    if not updated:
        print("  All checksums already up to date.")
        return

    # Write updated CHECKSUMS.json
    with open(checksums_path, 'w') as f:
        json.dump(checksums, f, indent=2)
        f.write('\n')
    print("  Updated CHECKSUMS.json")

    # Update .component_hash (SHA256 of CHECKSUMS.json)
    new_component_hash = compute_sha256(checksums_path)
    with open(component_hash_path, 'w') as f:
        f.write(new_component_hash)
    print(f"  Updated .component_hash: {new_component_hash}")


def apply_patches(project_dir):
    """Apply all patches to the managed esp-dl component."""
    component_dir = os.path.join(project_dir, 'managed_components', 'espressif__esp-dl')

    if not os.path.isdir(component_dir):
        print(f"ERROR: Component directory not found: {component_dir}")
        return False

    print(f"ESP-DL component directory: {component_dir}")

    # Define all patches: (file, marker, old_text, new_text)
    patches = [
        # Patch 1: dl_image_process.hpp - disable SIMD image transform on ESP32-P4
        # (workaround for "Load access fault" in dl_esp32p4_resize_nn_*.S, esp-dl #305)
        {
            'file': 'vision/image/dl_image_process.hpp',
            'marker': 'SIMD resize kernel raises a "Load access fault"',
            'old': '''        void operator()(const PixelCvt &pixel_cvt) const
        {
            constexpr bool simd = SIMD && !is_yuv_cvt_v<PixelCvt>;
            self->template transform_nn<PixelCvt, simd>(pixel_cvt);
        }''',
            'new': '''        void operator()(const PixelCvt &pixel_cvt) const
        {
            // ---- workaround ----
            // ESP32-P4: CONFIG_PIE_V2_BOOST forces the SIMD transform path
            // (dl_esp32p4_resize_nn_*.S). On some P4 silicon/memory layouts the
            // SIMD resize kernel raises a "Load access fault" while resizing
            // RGB888 source frames (upstream esp-dl issue #305, identical crash
            // signature MCAUSE=5/MTVAL=0). Fall back to the generic scalar path
            // used on the other targets, which performs the same bounded reads.
            constexpr bool simd = false; // SIMD && !is_yuv_cvt_v<PixelCvt>;
            self->template transform_nn<PixelCvt, simd>(pixel_cvt);
        }''',
        },
    ]

    # Check and apply each patch
    patches_applied = []
    for i, patch in enumerate(patches):
        filepath = os.path.join(component_dir, patch['file'])
        if not os.path.exists(filepath):
            print(f"  [{i+1}] SKIP: {patch['file']} not found")
            continue

        with open(filepath, 'r') as f:
            content = f.read()

        # Check if patch is already applied by looking for the marker
        if patch['marker'] in content:
            print(f"  [{i+1}] OK:   {patch['file']} - already patched")
            continue

        # Check if the old text exists
        if patch['old'] not in content:
            print(f"  [{i+1}] WARN: {patch['file']} - old text not found, skipping")
            continue

        # Apply the patch
        new_content = content.replace(patch['old'], patch['new'], 1)
        with open(filepath, 'w') as f:
            f.write(new_content)

        print(f"  [{i+1}] APPLIED: {patch['file']}")
        patches_applied.append(patch['file'])

    if not patches_applied:
        print("\nAll patches already applied. Verifying checksums...")
        verify_checksums(component_dir, patches)
        return True

    print(f"\nApplied {len(patches_applied)} patches. Updating checksums...")
    update_checksums(component_dir, patches)
    return True


def verify_checksums(component_dir, patches):
    """Verify and update checksums without applying patches."""
    checksums_path = os.path.join(component_dir, 'CHECKSUMS.json')
    component_hash_path = os.path.join(component_dir, '.component_hash')

    with open(checksums_path, 'r') as f:
        checksums = json.load(f)

    modified_files = set(p['file'] for p in patches)
    needs_update = False
    for entry in checksums['files']:
        if entry['path'] in modified_files:
            filepath = os.path.join(component_dir, entry['path'])
            if os.path.exists(filepath):
                actual_hash = compute_sha256(filepath)
                actual_size = os.path.getsize(filepath)
                if entry['hash'] != actual_hash or entry['size'] != actual_size:
                    print(f"  Checksum mismatch: {entry['path']}")
                    print(f"    Expected: {entry['hash']} ({entry['size']} bytes)")
                    print(f"    Actual:   {actual_hash} ({actual_size} bytes)")
                    entry['hash'] = actual_hash
                    entry['size'] = actual_size
                    needs_update = True

    if not needs_update:
        print("  All checksums match. No update needed.")
        return

    with open(checksums_path, 'w') as f:
        json.dump(checksums, f, indent=2)
        f.write('\n')
    print("  Updated CHECKSUMS.json")

    new_component_hash = compute_sha256(checksums_path)
    with open(component_hash_path, 'w') as f:
        f.write(new_component_hash)
    print(f"  Updated .component_hash: {new_component_hash}")


def main():
    if len(sys.argv) < 2:
        print("Usage: python apply_patches.py <project_dir>")
        sys.exit(1)

    project_dir = os.path.abspath(sys.argv[1])
    print(f"Project directory: {project_dir}")
    print("Checking ESP-DL patches for ESP32-P4 vision...")
    print()

    if not apply_patches(project_dir):
        sys.exit(1)

    print("\nDone.")


if __name__ == '__main__':
    main()
