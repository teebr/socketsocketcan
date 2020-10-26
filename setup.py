import glob
import os
import sys

import setuptools
from setuptools.command.build_ext import build_ext

with open('README.md', 'r') as fd:
    long_description = fd.read()


class get_pybind_include(object):
    """Helper class to determine the pybind11 include path

    The purpose of this class is to postpone importing pybind11
    until it is actually installed, so that the ``get_include()``
    method can be invoked. """

    def __str__(self):
        import pybind11
        return pybind11.get_include()


ext_modules = [
    setuptools.Extension(
        'tcpclient',
        sources=glob.glob(os.path.join(os.path.dirname(__file__), 'client', '*.cpp')),
        include_dirs=[
            # Path to pybind11 headers
            get_pybind_include(),
        ],
        define_macros=[('DEBUG', '0'), ('RECV_OWN_MSGS', '0'), ('WAIT_FOR_TCP_CONNECTION', '1')],
        language='c++'
    ),
]


# As of Python 3.6, CCompiler has a `has_flag` method.
# cf http://bugs.python.org/issue26689
def has_flag(compiler, flagname):
    """Return a boolean indicating whether a flag name is supported on
    the specified compiler.
    """
    import tempfile
    with tempfile.NamedTemporaryFile('w', suffix='.cpp') as f:
        f.write('int main (int argc, char **argv) { return 0; }')
        try:
            compiler.compile([f.name], extra_postargs=[flagname])
        except setuptools.distutils.errors.CompileError:
            return False
    return True


def cpp_flag(compiler):
    """Return the -std=c++[11/14/17] compiler flag.

    The c++17 is preferred over c++14 and c++11 (when it is available).
    """
    if has_flag(compiler, '-std=c++17'):
        return '-std=c++17'
    elif has_flag(compiler, '-std=c++14'):
        return '-std=c++14'
    elif has_flag(compiler, '-std=c++11'):
        return '-std=c++11'
    else:
        raise RuntimeError('Unsupported compiler -- at least C++11 support is needed!')


class BuildExt(build_ext):
    """A custom build extension for adding compiler-specific options."""
    c_opts = {
        'msvc': ['/EHsc'],
        'unix': [],
    }

    if sys.platform == 'darwin':
        c_opts['unix'] += ['-stdlib=libc++', '-mmacosx-version-min=10.7']

    def build_extensions(self):
        ct = self.compiler.compiler_type
        opts = self.c_opts.get(ct, [])
        if ct == 'unix':
            opts.append('-DVERSION_INFO="%s"' % self.distribution.get_version())
            opts.append(cpp_flag(self.compiler))
            if has_flag(self.compiler, '-fvisibility=hidden'):
                opts.append('-fvisibility=hidden')
        elif ct == 'msvc':
            opts.append('/DVERSION_INFO=\\"%s\\"' % self.distribution.get_version())
        for ext in self.extensions:
            ext.extra_compile_args = opts

        # Avoid gcc warning below:
        # cc1plus: warning: command line option ‘-Wstrict-prototypes’ is valid
        # for C/ObjC but not for C++
        if '-Wstrict-prototypes' in self.compiler.compiler_so:
            self.compiler.compiler_so.remove('-Wstrict-prototypes')

        build_ext.build_extensions(self)


setuptools.setup(
    name="socketsocketcan",
    version="0.0.2",
    author="Thomas Bruen, Kristian Sloth Lauszus",
    author_email="lauszus@gmail.com",
    description="Socketcan over TCP",
    long_description=long_description,
    long_description_content_type="text/markdown",
    ext_modules=ext_modules,
    install_requires=['python-can', 'psutil'],
    setup_requires=['pybind11>=2.4'],
    cmdclass={'build_ext': BuildExt},
    zip_safe=False,
    packages=setuptools.find_packages(),
    classifiers=[
        "Programming Language :: Python :: 3",
        "License :: OSI Approved :: MIT License",
        "Operating System :: OS Independent",
    ],
)
