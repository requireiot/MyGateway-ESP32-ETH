import subprocess,os,datetime

try:
    revision = (
        subprocess.check_output(["svnversion", "-n", "."])
        .strip()
        .decode("utf-8")
    )
except subprocess.CalledProcessError:
    exit()

print("'-DSVN_REV=\"%s\"'" % revision)

VERSION_HEADER = 'Revision.h'

HEADER_FILE = """
    // AUTO GENERATED FILE, DO NOT EDIT
    #ifndef SVN_REV
        #define SVN_REV "{}"
    #endif
    // last checked {}
    """.format(str(revision),datetime.datetime.now())

if os.environ.get('PLATFORMIO_INCLUDE_DIR') is not None:
    VERSION_HEADER = os.environ.get('PLATFORMIO_INCLUDE_DIR') + os.sep + VERSION_HEADER
elif os.path.exists("include"):
    VERSION_HEADER = "include" + os.sep + VERSION_HEADER
else:
    PROJECT_DIR = env.subst("$PROJECT_DIR")
    os.mkdir(PROJECT_DIR + os.sep + "include")
    VERSION_HEADER = "include" + os.sep + VERSION_HEADER

with open(VERSION_HEADER, 'w+') as FILE:
    FILE.write(HEADER_FILE)
