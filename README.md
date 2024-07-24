Microsoft Visual Studio Community 2022 (64 位) - Current
版本 17.10.3
编译通过
编译方式
1、vcpkg 安装以下包

.\vcpkg.exe install atl atlmfc boost brotli bzip2 dirent double-conversion egl-registry expat fftw3 freetype getopt-win32 getopt gettext-libintl gettext glib harfbuzz libffi libiconv libjpeg-turbo liblzma libpng libusb libwebp openssl pcre2 python3 qt5 sqlite3 tiff zlib zstd

注意 默认在D 盘根目录 安装vcpkg

2、clone 本工程, checkout -b dev_msvc 用visual studio 打开DSView.sln 直接编译 

3、 将dll1.zip  dll2.zip 解压到 D:\DSView\build.dir\Debug\ 
    虽然选的静态编译 但是运行的时候 仍然包却很多动态库，暂时不知道原因，dll.zip 时所有的库和其他以来选项

4、可以直接调试，还可以打断点，嘎嘎嘎