# Packaging

Two PKGBUILDs live here. `aur/nemdown-git` builds from the tip of the
repository; `aur/nemdown` builds from a tagged release tarball. They install
identical files and declare each other in `provides`/`conflicts`, so only one
can be installed at a time.

## Before publishing anything

Both PKGBUILDs carry `url='https://github.com/CHANGEME/nemdown'`. That field is
the package's **source** as well as its homepage, so it is the one value that
must be right or nothing builds. Set it, and make the repository public.

## Publishing nemdown-git

This is the one to start with: it needs no release tags.

```
# once: add an SSH public key at https://aur.archlinux.org/ under My Account
git clone ssh://aur@aur.archlinux.org/nemdown-git.git
cp packaging/aur/nemdown-git/{PKGBUILD,.SRCINFO} nemdown-git/
cd nemdown-git
makepkg --printsrcinfo > .SRCINFO     # regenerate; it must match the PKGBUILD
git add PKGBUILD .SRCINFO
git commit -m 'Initial import'
git push
```

`.SRCINFO` is not generated server-side. A commit whose `.SRCINFO` disagrees
with its `PKGBUILD` is rejected, so regenerate it after every edit.

## Publishing nemdown

Additionally requires a tag and a real checksum:

```
git tag -a v0.1.0 -m 'nemdown 0.1.0' && git push --tags
cd packaging/aur/nemdown
updpkgsums                            # replaces the sha256sums placeholder
makepkg --printsrcinfo > .SRCINFO
```

`sha256sums=('SKIP')` is a placeholder. Publishing it unchanged ships a
package nobody can verify, which defeats the point of shipping a checksum at
all.

## Testing a change

```
makepkg -f                            # build, run make test, package
namcap PKGBUILD *.pkg.tar.zst         # lint both
```

A plain `makepkg` uses the dependencies already on the machine, so it cannot
tell a real `makedepends` from something you happen to have installed. Only a
clean chroot can:

```
extra-x86_64-build                    # devtools; needs root
```

Run that before the first publish and after any dependency change.
