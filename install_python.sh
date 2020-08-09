rm -rf build
mkdir build
cd build
cmake ..
make -j32
cd ..
cd pyhton-package
python setup.py install --precompile
