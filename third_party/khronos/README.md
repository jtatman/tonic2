OpenGL extension headers from the Khronos Group (MIT license, see each file header).
DPF downloads these from khronos.org at configure time; tonic2 ships them and copies them into
third_party/DPF/khronos before configuring so builds do not depend on that download (it failed on CI).
