# Run this script to generate a new version of functions.py
# from the headers on your system

import os, sys, re
from optparse import OptionParser
from tempfile import NamedTemporaryFile
from glob import glob

parser = OptionParser()
parser.add_option("-a", "--apr-config", dest="apr_config",
                  help="The full path to your apr-1-config or apr-config script")
parser.add_option("-p", "--prefix", dest="prefix",
                  help="Specify the prefix where Subversion is installed (e.g. /usr, or /usr/local)")

(options, args) = parser.parse_args()

def get_preprocessor_flags():
    flags = []
    ldflags = []
    ld_library_path = []
    ferr = None
    prefix = options.prefix
    apr_include_dir = None
    apr_config_paths = ('apr-1-config', 'apr-config')
    if options.apr_config:
        apr_config_paths = (options.apr_config,)
    elif prefix:
        apr_config_paths = ('%s/bin/apr-1-config' % prefix,
                            '%s/bin/apr-config' % prefix,
                            'apr-1-config', 'apr-config')

    for apr_config in apr_config_paths:
        fout, ferr = run_cmd("%s --includes --cppflags" % apr_config)
        if fout:
            flags = fout.split()
            apr_prefix, _ = run_cmd("%s --prefix" % apr_config)
            apr_prefix = apr_prefix.strip()
            if not prefix:
                prefix = apr_prefix
            apr_include_dir, _ = run_cmd("%s --includedir" % apr_config)
            apr_include_dir = apr_include_dir.strip()
            break
    else:
        print ferr
        raise Exception("Cannot find apr-1-config or apr-config. Please specify\n"
                        "the full path to your apr-1-config or apr-config script\n"
                        "using the --apr-config option.")

    subversion_prefixes = [
        prefix,
        "/usr/local",
        "/usr"
    ]

    for svn_prefix in subversion_prefixes:
        svn_include_dir = "%s/include/subversion-1" % svn_prefix
        if os.path.exists("%s/svn_client.h" % svn_include_dir):
            flags.append("-I%s" % svn_include_dir)
            break
    else:
        print ferr
        raise Exception("Cannot find svn_client.h. Please specify the prefix\n"
                        "to your Subversion installation using the --prefix\n"
                        "option.")

    for fname in glob("%s/lib/libapr*-1.so" % apr_prefix):
        ldflags.append("-l%s" % fname)
    for fname in glob("%s/lib/libsvn_*.so" % svn_prefix):
        ldflags.append("-l%s" % fname)

    return (svn_prefix, apr_prefix, " ".join(ldflags), " ".join(flags),
            ":".join(ld_library_path))

def run_cmd(cmd):
    fin, fout, ferr = os.popen3(cmd)
    fin.close()
    return fout.read(), ferr.read()

(svn_prefix, apr_prefix, ldflags, flags, ld_library_path) = get_preprocessor_flags()
f = file("svn_all.h", "w")
for filename in glob("%s/include/subversion-1/svn_*.h" % svn_prefix):
    print >>f, "#include <%s>" % filename
f.close()

os.environ["LD_LIBRARY_PATH"] = ld_library_path
os.system("h2xml.py -c `pwd`/%s %s -o svn_all.xml" % (f.name, flags))
os.system("xml2py.py %s -r '^svn.*|^apr.*|^SVN_.*|^APR_.*' svn_all.xml -o svn_all.py" % ldflags)

func_re = re.compile(r"CFUNCTYPE\(POINTER\((\w+)\)")
out = file("svn_all2.py", "w")
for line in file("svn_all.py"):
    line = line.replace("CFUNCTYPE(POINTER(svn_error_t)",
                        "CFUNCTYPE(SVN_ERR_UNCHECKED")
    line = line.replace("restype = POINTER(svn_error_t)",
                        "restype = SVN_ERR")
    line = func_re.sub(r"CFUNCTYPE(POINTER_UNCHECKED(\1)", line)
    line = line.replace("'%s/lib/" % svn_prefix, "'")
    line = line.replace("'%s/lib/" % apr_prefix, "'")
    if not line.startswith("FILE ="):
        out.write(line)
out.close()
os.unlink("svn_all.h")
os.unlink("svn_all.xml")
os.unlink("svn_all.py")
os.system("cat csvn/core/functions.py.in svn_all2.py > csvn/core/functions.py")
os.unlink("svn_all2.py")
