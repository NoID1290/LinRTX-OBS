# Maintainer: NoID1290
pkgname=obs-plugin-linrtx
pkgver=1.0.0
pkgrel=1
pkgdesc="Native Linux OBS Studio plugin providing NVIDIA RTX Video Super Resolution (VSR) and TrueHDR filters via zero-copy CUDA-OpenGL interop"
arch=('x86_64')
url="https://github.com/NoID1290/LinRTX-OBS"
license=('GPL-2.0-or-later')
depends=('obs-studio>=30.0.0' 'cuda' 'nvidia-utils' 'libglvnd')
makedepends=('cmake' 'ninja' 'cuda' 'pkgconf' 'gcc')
optdepends=(
    'nvidia-vfx: NVIDIA Video Effects SDK runtime libraries from PyPI (recommended)'
    'comfyui-venv: ComfyUI Python environment with nvvfx'
)
provides=('obs-plugin-linrtx')
conflicts=('obs-plugin-linrtx-git')

build() {
    cmake -B build -S . \
        -G Ninja \
        -DCMAKE_BUILD_TYPE=Release \
        -DCMAKE_INSTALL_PREFIX=/usr \
        -DENABLE_VSR=ON \
        -DENABLE_TRUEHDR=ON
    cmake --build build
}

package() {
    DESTDIR="${pkgdir}" cmake --install build
}
