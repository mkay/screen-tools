pkgname=screen-tools
pkgver=0.2.0
pkgrel=1
pkgdesc="Crosshair guides, pixel measurement and a colour-picker loupe for Wayfire"
arch=('x86_64')
license=('MIT')
depends=(
  'wayfire>=0.11.0'
  'cairo'
)
optdepends=('wl-clipboard: copy picked colours to the clipboard')
makedepends=('meson' 'ninja')
source=()

build() {
  cd "$startdir"
  meson setup builddir --prefix=/usr --buildtype=plain --wipe
  ninja -C builddir
}

package() {
  cd "$startdir"
  DESTDIR="$pkgdir" meson install -C builddir --no-rebuild
}
