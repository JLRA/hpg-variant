
# Initialize the environment with path variables, CFLAGS, and so on
bioinfo_path = '#libs/bioinfo-libs'
commons_path = '#libs/common-libs'
math_path = '#libs/math'

env = Environment(tools = ['default', 'packaging'],
                  CFLAGS = '-std=c99 -D_XOPEN_SOURCE=600 -D_GNU_SOURCE -fopenmp',
                  CPPPATH = ['#', '#src', '#include', '/usr/local/include', '/usr/include/libxml2', bioinfo_path, commons_path, math_path ],
                  LIBPATH = ['/usr/lib', '/usr/local/lib', '#libs', '#libs/common-libs/', commons_path ],
                  LIBS = ['argtable2', 'common', 'config', 'cprops', 'curl', 'gsl', 'gslcblas', 'm', 'xml2', 'z'],
                  LINKFLAGS = ['-fopenmp'])
                  
if int(ARGUMENTS.get('debug', '0')) == 1:
    debug = 1
    env['CFLAGS'] += ' -O0 -g'
else:
    debug = 0
    env['CFLAGS'] += ' -O3'

env['objects'] = []


# Targets

SConscript(['%s/bioformats/SConscript' % bioinfo_path,
            '%s/SConscript' % commons_path,
            '%s/SConscript' % math_path
            ], exports = ['env', 'debug'])

SConscript(['src/effect/SConscript',
            'src/gwas/SConscript',
            'src/vcf-tools/SConscript'
            ], exports = ['env', 'debug', 'commons_path', 'bioinfo_path', 'math_path'])

env.Install('#bin', ['hpg-variant.cfg', 'vcf-info-fields.cfg'])

if 'debian' in COMMAND_LINE_TARGETS:
    SConscript("deb/SConscript", exports = ['env'] )