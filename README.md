# kubelet topology learning tool
















Helpers
git push -u origin main


apt update
apt install git hwloc python3 python3-pip python3-setuptools python3-wheel ninja-build 

pip3 install meson

meson buiddir
cd builddir
meson compile
