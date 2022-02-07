#!/bin/bash
set -eux -o pipefail


windows_s3_link="https://ossci-windows.s3.amazonaws.com"

case ${CUDA_VERSION} in
    10.1)
        # This is typically blank but for CUDA 10* it'll be set to 10
        cudnn_file_name="cudnn-${CUDA_VERSION}-windows10-x64-v7.6.4.38"
        ;;
    10.2)
        cudnn_file_name="cudnn-${CUDA_VERSION}-windows10-x64-v7.6.5.32"
        ;;
    11.1)
        cudnn_file_name="cudnn-${CUDA_VERSION}-windows-x64-v8.0.5.39"
        ;;
    11.3)
        cudnn_file_name="cudnn-${CUDA_VERSION}-windows-x64-v8.2.0.53"
        ;;
    11.5)
        # Since cudnn 8.3  the filename have changed
        cudnn_file_name="cudnn-windows-x86_64-8.3.2.44_cuda${CUDA_VERSION}-archive"
        ;;
    *)
        echo "CUDA_VERSION: ${CUDA_VERSION} not supported yet"
        exit 1
        ;;
esac

cudnn_installer_name="cudnn_installer.zip"
cudnn_installer_link="${windows_s3_link}/${cudnn_file_name}.zip"
cudnn_install_folder="C:/Program Files/NVIDIA GPU Computing Toolkit/CUDA/v${CUDA_VERSION}/"

if [[ -f "${cudnn_install_folder}/include/cudnn.h" ]]; then
    echo "Existing cudnn installation found, skipping install..."
else
    tmp_dir=$(mktemp -d)
    (
        pushd "${tmp_dir}"
        curl --retry 3 -o "${cudnn_installer_name}" "$cudnn_installer_link"
        7z x "${cudnn_installer_name}" -ocudnn
        # Use '${var:?}/*' to avoid potentially expanding to '/*'
        # Remove all of the directories before attempting to copy files
        rm -rf "${cudnn_install_folder:?}/*"
        cp -rf cudnn/cuda/* "${cudnn_install_folder}"

        #Make sure windows path contains zlib dll
        curl -k -L "${windows_s3_link}/zlib123dllx64.zip" --output "${tmp_dir}\zlib123dllx64.zip"
        7z x "${tmp_dir}\zlib123dllx64.zip" -o"${tmp_dir}\zlib"
        xcopy /Y "${tmp_dir}\zlib\dll_x64\*.dll" "C:\Windows\System32"
    )
    rm -rf "${tmp_dir}"
fi
