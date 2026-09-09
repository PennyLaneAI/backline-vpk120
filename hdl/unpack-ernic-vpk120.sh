#!/bin/bash
# SPDX-License-Identifier: Apache-2.0
# Copyright 2026 Xanadu Quantum Technologies Inc.

ARCH="reference design VPK120_mrmac_rev_1.3.zip"
TEMP_DIR="tmp_extract"
TARGET_HW_DIR="ernic-vpk120/hw"
TARGET_SW_DIR="ernic-vpk120/sw"
TARGET_DOCS_DIR="ernic-vpk120/docs"

BASE_PATH="reference design VPK120_mrmac_rev_1.3/reference_designs_VPK120_mrmac/designs/2024.2/vpk120m_ernic_ref_design"
HW_SUBPATH="$BASE_PATH/hw/hw"
SW_SUBPATH="$BASE_PATH/sw"
DOC_SUBPATH="$BASE_PATH/doc/doc"
SW_ZIP_SUBPATH="$TARGET_SW_DIR/ERNIC-2024_2_VPK120_RC6"
INNER_ZIP="$TARGET_SW_DIR/ERNIC-2024_2_VPK120_RC6.zip"
INNER_DIR="$TARGET_SW_DIR/ERNIC-2024_2_VPK120_RC6"
APPLY_PATCH=false

if [[ "${1:-}" == "--patch" ]] || [[ "${1:-}" == "-p" ]]; then
    APPLY_PATCH=true
fi

if [ ! -f "$ARCH" ]; then
    echo "ERROR: Can't find reference design sources (file $ARCH). Please put it in this directory and run me again."
    exit 1
fi

mkdir -p "$TARGET_HW_DIR" "$TARGET_SW_DIR" "$TARGET_DOCS_DIR" "$TEMP_DIR"

unzip -q "$ARCH" "$HW_SUBPATH/*" "$SW_SUBPATH/*" "$DOC_SUBPATH/*" -d "$TEMP_DIR"

cp -r "$TEMP_DIR/$HW_SUBPATH/"* "$TARGET_HW_DIR/"
cp -r "$TEMP_DIR/$SW_SUBPATH/"* "$TARGET_SW_DIR/"
cp -r "$TEMP_DIR/$DOC_SUBPATH/"* "$TARGET_DOCS_DIR/"

unzip -q "$INNER_ZIP" -d "$TARGET_SW_DIR"
rm "$INNER_ZIP"
mv "$INNER_DIR"/* "$TARGET_SW_DIR/"
rmdir "$INNER_DIR"

rm -rf "$TEMP_DIR"

sed -i '1908,1914d' "ernic-vpk120/hw/local_ip_cores/hw_handshake_v1_0/component.xml"
sed -i '1807,1841d' "ernic-vpk120/hw/local_ip_cores/hw_handshake_v1_0/component.xml"
sed -i '1816,1850d' "ernic-vpk120/hw/local_ip_cores/hw_handshake_v1_0/component.xml"
sed -i '183,$d' "ernic-vpk120/hw/local_ip_cores/hw_handshake_v1_0/hdl/hw_handshake_v1_0.v"

if [ "$APPLY_PATCH" = true ]; then
    if [ -f "backline.patch" ]; then
        echo "Applying patch: backline.patch"
        patch -p1 -d ernic-vpk120 < backline.patch
    else
        echo "ERROR: No patch file (backline.patch or backline_payload.json) found." >&2
        exit 1
    fi
else
    echo "Unpacked clean staging directory to 'ernic-vpk120/'."
fi
