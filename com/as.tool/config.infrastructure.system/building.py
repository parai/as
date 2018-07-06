import os
import sys
import shutil
import string
import re
from SCons.Script import *
import xcc
import argen

Env = None

BuildOptions = {}

class Win32Spawn:
    def spawn(self, sh, escape, cmd, args, env):
        # deal with the cmd build-in commands which cannot be used in
        # subprocess.Popen
        if cmd == 'del':
            for f in args[1:]:
                try:
                    os.remove(f)
                except Exception as e:
                    print('Error removing file: %s'%(e))
                    return -1
            return 0

        import subprocess

        newargs = ' '.join(args[1:])
        cmdline = cmd + " " + newargs

        # Make sure the env is constructed by strings
        _e = dict([(k, str(v)) for k, v in env.items()])

        # Windows(tm) CreateProcess does not use the env passed to it to find
        # the executables. So we have to modify our own PATH to make Popen
        # work.
        old_path = os.environ['PATH']
        os.environ['PATH'] = _e['PATH']

        try:
            _e['PATH'] = Env['EXTRAPATH']+';'+_e['PATH']
        except KeyError:
            pass

        try:
            proc = subprocess.Popen(cmdline, env=_e, shell=False)
        except Exception as e:
            print('Error in calling:\n%s'%(cmdline))
            print('Exception: %s: %s'%(e, os.strerror(e.errno)))
            return e.errno
        finally:
            os.environ['PATH'] = old_path

        return proc.wait()

def PrepareRTTHREAD(opt):
    global BuildOptions
    BuildOptions = opt

def IsPlatformWSL():
    infor = RunSysCmd('uname -a')
    if('Microsoft' in str(infor)):
        return True
    return False

def IsPlatformWindows():
    bYes = False
    if(os.name == 'nt'):
        bYes = True
    if(sys.platform == 'msys'):
        bYes = True
    return bYes

def AppendPythonPath(lp):
    try:
        pypath = os.environ['PYTHONPATH']
    except KeyError:
        pypath = ''
    sep = ':'
    if(IsPlatformWindows()):
        sep = ';'
    for l in lp:
        pypath += sep+os.path.abspath(l)
    os.environ['PYTHONPATH'] = pypath

def PrepareEnv(release):
    ASROOT=os.path.abspath('%s/../..'%(os.curdir))
    BOARD=None

    AppendPythonPath(['%s/com/as.tool/config.infrastructure.system'%(ASROOT),
              '%s/com/as.tool/config.infrastructure.system/third_party'%(ASROOT)])

    asenv=Environment(TOOLS=['as','gcc','g++','gnulink'])
    asenv['ASROOT'] = ASROOT
    asenv['RELEASE'] = release
    board_list = []
    for dir in os.listdir('%s/com/as.application'%(ASROOT)):
        if(dir[:6]=='board.' and 
           os.path.exists('%s/com/as.application/%s/SConscript'%(ASROOT,dir))):
            board_list.append(dir[6:])

    def help():
        print('Usage:scons board [studio]\n\tboard:%s'%(board_list))
        print('  studio: optional for launch studio GUI tool')

    if('help' in COMMAND_LINE_TARGETS):
        help()
        exit(0)
    else:
        for b in COMMAND_LINE_TARGETS:
            if(b in board_list):
                BOARD = b

    if(BOARD is None):
        if(os.getenv('BOARD') in board_list):
            BOARD = os.getenv('BOARD')

    if(BOARD is None):
        print('Error: no BOARD specified!')
        if(IsPlatformWindows()):
            print('use DOS "set" command to set BOARD\n\tfor example: set BOARD=posix')
        help()
        exit(-1)

    asenv['BOARD'] = BOARD
    Export('asenv')
    PrepareBuilding(asenv)
    return asenv

def PrepareBuilding(env):
    global Env
    Env = env
    GetConfig('.config',env)
    env['pkgconfig'] = 'pkg-config'
    env['msys2'] = False
    env['POSTACTION'] = []
    if(IsPlatformWindows() and (env['CONFIGS'] != None)):
        if('PLATFORM_MINGW' in env['MODULES']):
            env['CC'] = env['CONFIGS']['MINGW_GCC_PATH'] + '/gcc'
            env['pkgconfig'] = env['CONFIGS']['MINGW_GCC_PATH'] + '/pkg-config'
            env['EXTRAPATH'] = env['CONFIGS']['MINGW_GCC_PATH']
        elif('PLATFORM_MSYS2' in env['MODULES']):
            env['msys2'] = True
            mpath = env['CONFIGS']['MSYS2_GCC_PATH']
            env['CC'] = mpath + '/gcc'
            env['pkgconfig'] = mpath + '/pkg-config'
            env['EXTRAPATH'] = '%s;%s'%(mpath, os.path.abspath(mpath+'/../usr/bin'))
    elif(IsPlatformWindows()):
        uname = RunSysCmd('uname')
        if('MSYS_NT' in str(uname)):
            print('build on %s, default assume 64 bit machine'%(uname.strip()))
            env['msys2'] = True
            env['pkgconfig'] = 'c:/msys64/mingw64/bin/pkg-config'
            env['CC'] = 'c:/msys64/mingw64/bin/gcc'
            env['LINK'] = 'c:/msys64/mingw64/bin/gcc'
            env['EXTRAPATH'] = 'c:/msys64/mingw64/bin;c:/msys64/usr/bin'
    env['python3'] = 'python3'
    if(IsPlatformWindows()):
        env['python3'] = 'python'
        env.AppendENVPath('PATH', os.getenv('PATH'))
        win32_spawn = Win32Spawn()
        env['SPAWN'] = win32_spawn.spawn
    env['CXX'] = env['CC']
    # add comstr option
    AddOption('--verbose',
            dest='verbose',
            action='store_true',
            default=False,
            help='print verbose information during build')
    AddOption('--force',
            dest='force',
            action='store_true',
            default=False,
            help='force rebuild of all')
    AddOption('--menuconfig',
            dest = 'menuconfig',
            action = 'store_true',
            default = False,
            help = 'make menuconfig for Automotive Software AS')
    AddOption('--splint',
            dest = 'splint',
            action = 'store_true',
            default = False,
            help = 'enanle splint for Automotive Software AS')

    if(not GetOption('verbose')):
    # override the default verbose command string
        env.Replace(
          ARCOMSTR = 'AR $SOURCE',
          ASCOMSTR = 'AS $SOURCE',
          ASPPCOMSTR = 'AS $SOURCE',
          CCCOMSTR = 'CC $SOURCE',
          CXXCOMSTR = 'CXX $SOURCE',
          LINKCOMSTR = 'LINK $TARGET'
        )
    if(GetOption('menuconfig')):
        menuconfig(env)

    if(0 != os.system('%s --version'%(env['pkgconfig']))):
        raise Exception('no pkg-config installed, fix the path maybe!')
    if(0 != os.system('%s --version'%(env['CC']))):
        raise Exception('no C Compiler installed, fix the path maybe!')

def mk_rtconfig(filename):
    try:
        config = open(filename)
    except:
        print('open %s failed'%(filename))
        return
    rtcfg = os.path.abspath('../../com/as.infrastructure/system/kernel/rtthread/menuconfig/rtconfig.h')
    if(not os.path.exists(rtcfg)):
        print('%s is not exits!'%(rtcfg))
    rtconfig = open(rtcfg, 'w')
    rtconfig.write('#if !defined(RT_CONFIG_H__)\n')
    rtconfig.write('#define RT_CONFIG_H__\n\n')

    empty_line = 1

    for line in config:
        line = line.lstrip(' ').replace('\n', '').replace('\r', '')

        if(len(line) == 0): continue

        if(line[0] == '#'):
            if len(line) == 1:
                if empty_line:
                    continue

                rtconfig.write('\n')
                empty_line = 1
                continue

            comment_line = line[1:]
            if(line.startswith('# CONFIG_')): line = ' ' + line[9:]
            else: line = line[1:]

            rtconfig.write('/*%s */\n' % line)
            empty_line = 0
        else:
            empty_line = 0
            setting = line.split('=')
            if(len(setting) >= 2):
                if(setting[0].startswith('CONFIG_')):
                    setting[0] = setting[0][7:]

                # remove CONFIG_PKG_XX_PATH or CONFIG_PKG_XX_VER
                if(type(setting[0]) == type('a') and (setting[0].endswith('_PATH') or setting[0].endswith('_VER'))):
                    continue

                if(setting[1] == 'y'):
                    rtconfig.write('#define %s\n' % setting[0])
                else:
                    rtconfig.write('#define %s %s\n' % (setting[0], setting[1]))

    rtconfig.write('\n')
    rtconfig.write('#endif\n')
    rtconfig.close()
    print('update %s done!'%(rtcfg))

def GetConfig(cfg,env):
    import re
    if(not os.path.exists(cfg)):
        # None to use default confiuration
        env['MODULES'] = None
        env['CONFIGS'] = None
        print('WARNING: no menuconfig file(".config") found, will use default configuration!')
        return
    env['MODULES'] = []
    env['CONFIGS'] = {}
    reOne = re.compile(r'([^\s]+)\s*=\s*([^\s]+)')
    fp = open(cfg)
    for el in fp.readlines():
        if(reOne.search(el)):
            name,value = reOne.search(el).groups()
            name = name.replace('CONFIG_','')
            if(value=='y'):
                env['MODULES'].append(name)
            else:
                if(value[0]=='"'): value = value[1:]
                if(value[-1]=='"'): value = value[:-1]
                env['CONFIGS'][name]=value
    fp.close()
    print('Modules:',env['MODULES'])
    print('Configs:',env['CONFIGS'])

def menuconfig(env):
    import time
    kconfig = '%s/com/as.tool/kconfig-frontends/kconfig-mconf'%(env['ASROOT'])
    if(IsPlatformWindows()):
        kconfig += '.exe'
        cmd2  = 'cd %s/com/as.tool/kconfig-frontends'%(env['ASROOT'])
        kurl = 'http://distortos.org/files/kconfig-frontends-3.12.0-windows.7z'
        cmd2 += ' && curl -O %s'%(kurl)
        for disk in ['C:/','D:/','E:/','F:/']:
            for prg in ['Program Files (x86)','Program Files','ProgramData']:
                _7z = os.path.join(disk, prg, '7-Zip/7z.exe')
                if(os.path.exists(_7z)): break
            if(os.path.exists(_7z)): break
        if(not os.path.exists(_7z)):
            raise Exception('Please Install 7z(https://www.7-zip.org/download.html)')
        cmd2 += ' && "%s" e kconfig-frontends-3.12.0-windows.7z'%(_7z)
        if(not os.path.exists(kconfig)):
            RunCommand(cmd2)
        if(not os.path.exists(kconfig)):
            raise Exception('please download %s and extract it as %s'%(kurl, kconfig))
        cmd = 'set BOARD=%s && set ASROOT=%s && '%(env['BOARD'],env['ASROOT'])
    else:
        cmd = 'export BOARD=%s && export ASROOT=%s && '%(env['BOARD'],env['ASROOT'])
    if(not os.path.exists(kconfig)):
        RunCommand('cd %s/com/as.tool/kconfig-frontends && make'%(env['ASROOT']))
    if(os.path.exists(kconfig)):
        assert(os.path.exists('Kconfig'))
        cmd += kconfig + ' Kconfig'

        fn = '.config'
        if(os.path.isfile(fn)):
            mtime = os.path.getmtime(fn)
        else:
            mtime = -1
        if(IsPlatformWindows()):
            cmd = '@echo off\n'+cmd.replace(' && ','\n')
            MKFile('menuconfig.bat', cmd)
            cmd = 'menuconfig.bat'
        RunCommand(cmd)
        if(os.path.isfile(fn)):
            mtime2 = os.path.getmtime(fn)
        else:
            mtime2 = -1
        if(mtime != mtime2):
            GetConfig(fn,env)
            cfgdir = 'build/%s/config'%(env['BOARD'])
            MKDir(cfgdir)
            xcc.XCC(cfgdir,env)
        if('RTTHREAD' in env['MODULES']):
            rtt = '%s/com/as.infrastructure/system/kernel/rtthread'%(env['ASROOT'])
            rttf = '%s/release/download/rt-thread'%(env['ASROOT'])
            if(not os.path.exists(rttf)):
                RunCommand('cd %s/release/download && git clone https://github.com/RT-Thread/rt-thread.git'%(env['ASROOT']))
            MKSymlink(rttf, rtt+'/rt-thread')
            fn = rtt+'/menuconfig/.config'
            if(os.path.isfile(fn)):
                mtime = os.path.getmtime(fn)
            else:
                mtime = -1
            cmd = 'cd %s/menuconfig && %s Kconfig'%(rtt, kconfig)
            if(IsPlatformWindows()):
                cmd = '@echo off\n'+cmd.replace(' && ','\n')
                MKFile('rtmenuconfig.bat', cmd)
                cmd = 'rtmenuconfig.bat'
            RunCommand(cmd)
            if(os.path.isfile(fn)):
                mtime2 = os.path.getmtime(fn)
            else:
                mtime2 = -1
            if(mtime != mtime2):
                mk_rtconfig(fn)
        exit(0)
    else:
        raise Exception("can't find out %s"%(kconfig))

def GetCurrentDir():
    conscript = File('SConscript')
    fn = conscript.rfile()
    path = os.path.dirname(fn.abspath)
    return path

def MKDir(p):
    ap = os.path.abspath(p)
    try:
        os.makedirs(ap)
    except:
        if(not os.path.exists(ap)):
            raise Exception('Fatal Error: can\'t create directory <%s>'%(ap))

def RMDir(p):
    if(os.path.exists(p)):
        shutil.rmtree(p)

def RMFile(p):
    if(os.path.exists(p)):
        print('removing %s'%(os.path.abspath(p)))
        os.remove(os.path.abspath(p))

def CURL(url, tgt=None):
    # curl is better than wget on msys2
    if(tgt == None):
        tgt = url.split('/')[-1]
    if(not os.path.exists(tgt)):
        RunCommand('curl %s -o %s'%(url,tgt))

def MKObject(src, tgt, cmd, rm=True):
    if(GetOption('clean') and rm):
        RMFile(tgt)
        return
    mtime = 0
    for s in src:
        s = str(s)
        if(os.path.isfile(s)):
            tm = os.path.getmtime(s)
            if(tm > mtime):
                mtime = tm
    if(os.path.isfile(tgt)):
        mtime2 = os.path.getmtime(tgt)
    else:
        mtime2 = -1
    if(mtime2 < mtime):
        RunCommand(cmd)

def MKFile(p,c='',m='w'):
    f = open(p,m)
    f.write(c)
    f.close()

def MKSymlink(src,dst):
    asrc = os.path.abspath(src)
    adst = os.path.abspath(dst)
    if(not os.path.exists(dst)):
        if(IsPlatformWindows()):
            if((sys.platform == 'msys') and
               (os.getenv('MSYS') == 'winsymlinks:nativestrict')):
                RunCommand('ln -fs %s %s'%(asrc,adst))
            elif(os.path.isdir(asrc)):
                RunCommand('mklink /D %s %s'%(adst,asrc))
            else:
                RunCommand('mklink %s %s'%(adst,asrc))
        else:
            os.symlink(asrc,adst)

def SrcRemove(src, remove):
    if not src:
        return

    for item in src:
        if type(item) == type('str'):
            if(os.path.basename(item) in remove):
                src.remove(str(item))
        else:
            if(os.path.basename(item.rstr()) in remove):
                src.remove(item)

def RunCommand(cmd):
    import subprocess
    if(GetOption('verbose')):
        print(' >> RunCommand "%s"'%(cmd))
    if(os.name == 'nt'):
        cmd = cmd.replace('&&', '&')
    if(0 != os.system(cmd)):
        raise Exception('FAIL of RunCommand "%s"'%(cmd))

def RunSysCmd(cmd):
    import subprocess
    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, shell=True)
    (output, err) = p.communicate()
    p_status = p.wait()
    return output

def DefineGroup(name, src, depend, **parameters):
    global Env
    if not GetDepend(depend):
        return []
    if('CCFLAGS' in parameters):
        Env.AppendUnique(CCFLAGS = parameters['CCFLAGS'])
    if('CPPPATH' in parameters):
        Env.AppendUnique(CPPPATH = parameters['CPPPATH'])
    if('CPPDEFINES' in parameters):
        Env.AppendUnique(CPPDEFINES = parameters['CPPDEFINES'])
    if('LINKFLAGS' in parameters):
        Env.AppendUnique(LINKFLAGS = parameters['LINKFLAGS'])
    objs = []    
    for obj in src:
        objs.append(Glob(str(obj)))
    return objs

def AddDepend(option):
    BuildOptions[option] = 1

def GetDepend(depend):
    building = True
    if(type(depend) == type('str')):
        if((not (depend in BuildOptions)) or (BuildOptions[depend] == 0)):
            building = False
        elif(BuildOptions[depend] != ''):
            return BuildOptions[depend]

        return building

    # for list type depend
    for item in depend:
        if(item != ''):
            if((not (item in BuildOptions)) or (BuildOptions[item] == 0)):
                building = False

    return building

def GetConfigValue(name):
    try:
        return BuildOptions[name]
    except:
        return ''

def GetELFEnv(so=True):
    global Env
    mmap = '%s/release/%s/build/%s/%s.map'%(Env['ASROOT'], Env['RELEASE'],
                Env['BOARD'],Env['BOARD'])
    cstr = ''
    if(os.path.exists(mmap) and (so==False)):
        # for executale files, should know all basic API address
        reSym = re.compile(r'^\s+(0x\w+)\s+([a-zA-Z_]\w+)$')
        ignore = ['main']
        fp = open(mmap)
        for el in fp.readlines():
            if(reSym.search(el)):
                grp = reSym.search(el).groups()
                if(grp[1] in ignore):continue
                cstr += '%s = %s;\n'%(grp[1], grp[0])
        fp.close()
    elif(so == False):
        return None
    cwd = os.path.abspath(os.path.curdir)
    if(Env.GetOption('clean')):
        RMFile('aself.lds')
    if(not os.path.exists('aself.lds')):
        MKFile('aself.lds',
               '''SECTIONS
{
  . = 0;
  .text : { *(.text*) }
  . = ALIGN(4);
  .data : { *(.data*) }
  . = ALIGN(4);
  .bss : { *(.bss*) }
  . = ALIGN(4);
  .rodata : { *(.rodata*) }
  . = ALIGN(4);
  .rel.plt : { *(.rel.*) }
}\n\n%s\n'''%(cstr), 'w')
    CPPPATH = []
    for p in Env['CPPPATH']:
        if('rt-thread' not in p):
            CPPPATH.append(p)
    env = Environment(CC=Env['CC'],
                      LINK=Env['LINK'],
                      CPPPATH=CPPPATH,
                      CCFLAGS=Env['CCFLAGS']+['-fPIC'],
                      LINKFLAGS=['-e','main','-fPIC','-s','-nostdlib','-T','%s/aself.lds'%(cwd)],
                      SHLINKFLAGS=['-fPIC','-shared','-s','-nostdlib',
                                   '-T','%s/aself.lds'%(cwd)],
                      TOOLS = Env['TOOLS'])
    for flg in ['-ffunction-sections','-fdata-sections','-g']:
        if(flg in env['CCFLAGS']):
            env['CCFLAGS'].remove(flg)
    if(not GetOption('verbose')):
    # override the default verbose command string
        env.Replace(
          ARCOMSTR = 'AR $SOURCE',
          CCCOMSTR = 'ELFCC $SOURCE',
          CXXCOMSTR = 'ELFCXX $SOURCE',
          LINKCOMSTR = 'ELFLINK $TARGET',
          SHCCCOMSTR = 'SHCC $SOURCE',
          SHCXXCOMSTR = 'SHCXX $SOURCE',
          SHLINKCOMSTR = 'SHLINK $TARGET'
        )
    if(IsPlatformWindows()):
        env['SHLINKCOM'] = '$SHLINK $SHLINKFLAGS $SOURCES -o $TARGET'
    return env

class Qemu():
    def __init__(self, qemu=None):
        arch_map = {'x86':'i386','cortex-m':'arm', 'arm64':'aarch64'}
        ASROOT = Env['ASROOT']
        ARCH = Env['ARCH']
        self.arch = Env['arch']
        self.params = ''
        self.params = '-serial tcp:127.0.0.1:1103,server'
        if('gdb' in COMMAND_LINE_TARGETS):
            self.params += ' -gdb tcp::1234 -S'
        if(self.arch in arch_map.keys()):
            self.arch = arch_map[self.arch]
        if(qemu is None):
            self.isAsQemu = True
            self.qemu = self.LocateASQemu()
            self.CreateDiskImg('%s/release/%s/asblk0.img'%(ASROOT,Env['RELEASE']), 32*1024*1024, 'vfat')
            self.CreateDiskImg('%s/release/%s/asblk1.img'%(ASROOT,Env['RELEASE']), 32*1024*1024, 'ext4')
        else:
            self.isAsQemu = False
            self.qemu = qemu

    def LocateASQemu(self):
        ASROOT = Env['ASROOT']
        candrvsrc = '%s/com/as.tool/lua/can/socketwin_can_driver.c'%(ASROOT)
        candrvtgt = '%s/com/as.tool/lua/script/socketwin_can_driver.exe'%(ASROOT)
        cmd = '%s -I%s/com/as.infrastructure/include -D__SOCKET_WIN_CAN_DRIVER__ %s -o %s'%(Env['CC'], ASROOT, candrvsrc, candrvtgt)
        if(IsPlatformWindows()):
            cmd += ' -D__WINDOWS__ -lwsock32'
        else:
            cmd += ' -D__LINUX__'
        MKObject(candrvsrc, candrvtgt, cmd)
        if(IsPlatformWindows()):
            # try default install location of qemu
            try:
                qemu = '%s/qemu-system-%s'%(Env['CONFIGS']['MSYS2_GCC_PATH'],self.arch)
            except:
                qemu = '%s/qemu-system-%s'%(Env['CC'][:-3],self.arch)
            if(not os.path.exists(qemu+'.exe')):
                qemu = '%s/com/as.tool/qemu/src/build-x86_64-w64-mingw32/%s-softmmu/qemu-system-%s'%(ASROOT, self.arch, self.arch)
        else:
            qemu = '%s/release/download/qemu/%s-softmmu/qemu-system-%s'%(ASROOT, self.arch, self.arch)
        if(IsPlatformWindows()):
            qemu += '.exe'
        if(not os.path.exists(qemu)):
            print('%s is not exits, try build it out locally!'%(qemu))
            self.BuildASQemu()
        self.params += ' -device pci-ascan -device pci-asnet -device pci-asblk'
        return qemu

    def Run(self, params, where=None):
        ASROOT = Env['ASROOT']
        build = '%s/release/%s'%(ASROOT, Env['RELEASE'])
        if(where is None):
            where = build
        python = Env['python3']
        if(IsPlatformWindows()):
            python = 'start ' + python
        if('asone' in COMMAND_LINE_TARGETS):
            RunCommand('cd %s/com/as.tool/as.one.py && %s main.py'%(ASROOT,python))
        if(IsPlatformWindows()):
            if(self.isAsQemu):
                RunCommand('start %s/com/as.tool/lua/script/socketwin_can_driver.exe 0'%(ASROOT))
                RunCommand('start %s/com/as.tool/lua/script/socketwin_can_driver.exe 1'%(ASROOT))
            RunCommand('cd %s && start cmd /C %s %s %s'%(where, self.qemu, params, self.params))
            RunCommand('sleep 2 && telnet 127.0.0.1 1103')
        else:
            fp = open('%s/telnet.sh'%(build),'w')
            fp.write('sleep 0.5\ntelnet 127.0.0.1 1103\n')
            fp.close()
            fp = open('%s/qemu.sh'%(build),'w')
            fp.write('%s %s %s & sh %s/telnet.sh\nsleep 60\n'%(self.qemu,params,self.params,build))
            fp.close()
            RunCommand('sudo pgrep qemu-system-%s | xargs -i kill -9 {}'%(self.arch))
            RunCommand('cd %s && chmod +x %s/*.sh && sudo gnome-terminal -x %s/qemu.sh'%(build,build,build))
        exit(0)

    def CreateDiskImg(self, file, size, type='raw'):
        ASROOT = Env['ASROOT']
        if(os.path.exists(file)):
            print('DiskImg "%s" already exist!'%(file))
            return
        print('Create a New DiskImg "%s"!'%(file))
        if(IsPlatformWindows()):
            # try default install location of qemu
            try:
                qemuimg = '%s/qemu-img'%(Env['CONFIGS']['MSYS2_GCC_PATH'])
            except:
                qemuimg = '%s/qemu-img'%(Env['CC'][:-3])
            if(not os.path.exists(qemuimg+'.exe')):
                qemuimg = '%s/com/as.tool/qemu/src/build-x86_64-w64-mingw32/qemu-img'%(ASROOT)
        else:
            qemuimg = '%s/release/download/qemu/qemu-img'%(ASROOT)
            if(not os.path.exists(qemuimg)):
                qemuimg = 'qemu-img'

        RunCommand('%s create -f raw %s %s'%(qemuimg, file, size))

        if(type.startswith('ext')):
            if(IsPlatformWindows()):
                lwext4mkfs = '%s/release/download/lwext4/build_generic/fs_test/lwext4-mkfs.exe'%(ASROOT)
                RunCommand('%s -i %s -b 4096 -e %s'%(lwext4mkfs,file,type[3]))
            else:
                RunCommand('sudo mkfs.%s -b 4096 %s'%(type,file))
        elif(type.startswith('vfat')):
            if(IsPlatformWindows()):
                pass # TODO
            else:
                RunCommand('sudo mkfs.fat %s'%(file))

    def BuildASQemu(self):
        ASROOT = Env['ASROOT']
        if(IsPlatformWindows()):
            mpath = os.path.abspath(Env['CONFIGS']['MSYS2_GCC_PATH']+"/../..")
            RunCommand('%s/msys2_shell.cmd -mingw64 -where %s/com/as.tool/qemu'%(mpath,ASROOT))
            print('please mannuly invoke below comand in the poped up msys2 window:')
            print('\tMINGW_INSTALLS=mingw64 makepkg-mingw -sLf')
            print('\tpacman -U mingw-w64-x86_64-qemu-2.10.0-1-any.pkg.tar.xz')
            print('and then do "scons run" again')
            exit(-1)
        else:
            RunCommand('cd %s/release/ascore && make asqemu'%(ASROOT))

# accroding to http://benno.id.au/blog/2006/08/27/filtergensplint
# scons --splint -i  >  splint.log 2>&1
class splint():
    global Env
    def __init__(self, objs, env):
        self.update(env)

    def update(self,env):
        p = self.getit()
        for m in Env['MODULES']:
            env.Append(CPPDEFINES=['USE_%s'%(m)])
        env.Append(CPPDEFINES=['__GNUC__'])
        if(IsPlatformWindows()):
            RunCommand('set LARCH_PATH=%s/lib'%(p))
        os.environ['LARCH_PATH'] ='%s/lib'%(p)
        env['CC'] = '%s/bin/splint'%(p)
        env.Append(CFLAGS=['-badflag','-likelybool','-nestcomment','-preproc'])
        env['CCCOM'] = '$CC $CFLAGS $CCFLAGS $_CCCOMCOM $SOURCES'

    def getit(self):
        ASROOT = Env['ASROOT']
        if(IsPlatformWindows()):
            pkg = 'https://github.com/downloads/maoserr/splint_win32/splint-3.1.2.zip'
            lintdir = 'splint-3.1.2'
        else:
            pkg = 'http://www.splint.org/downloads/splint-3.1.2.src.tgz'
            lintdir = 'splint-3.1.2'
        if(not os.path.exists('%s/release/download/%s'%(ASROOT,lintdir))):
            RunCommand('cd %s/release/download && curl -O %s'%(ASROOT,pkg))
            if(IsPlatformWindows()):
                RunCommand('cd %s/release/download && unzip %s'%(ASROOT,os.path.basename(pkg)))
            else:
                RunCommand('cd %s/release/download && tar xf %s && cd %s && ./configure && make'%(ASROOT,os.path.basename(pkg),lintdir))
        return '%s/release/download/%s'%(ASROOT,lintdir)

def SelectCompilerARMICC(iarPath=None):
    import glob
    iar = iarPath
    if(iar is None):
        # try search path of IAR
        for disk in ['C:/','D:/','E:/','F:/']:
            for prg in ['Program Files (x86)','Program Files','ProgramData']:
                iar = os.path.join(disk, prg, 'IAR Systems')
                if(os.path.exists(iar)):
                    for bench in glob.glob(os.path.join(iar,'*')):
                        iar = os.path.join(bench,'arm')
                        if(os.path.exists(os.path.join(iar,'bin','iccarm.exe'))):
                            print('IAR:: %s'%(iar))
                            iarPath = iar
    if(iarPath is not None):
        print('IAR:: %s <== using this one'%(iarPath))
        Env['CC']=os.path.join(iarPath,'bin','iccarm.exe')
        Env['AS']=os.path.join(iarPath,'bin','iasmarm.exe')
        Env['LINK']=os.path.join(iarPath,'bin','ilinkarm.exe')
        Env['AR']=os.path.join(iarPath,'bin','iarchive.exe')

    return iarPath

def SelectCompilerArmNoneEabi():
    global Env
    ASROOT = Env['ASROOT']
    Env['CC']='arm-none-eabi-gcc -std=gnu99'
    Env['CXX']='arm-none-eabi-g++'
    Env['AS']='arm-none-eabi-as'
    Env['LINK']='arm-none-eabi-ld'
    Env['S19'] = 'arm-none-eabi-objcopy -O srec --srec-forceS3 --srec-len 32'
    if(IsPlatformWindows()):
        gccarm = 'gcc-arm-none-eabi-5_4-2016q3-20160926-win32'
        gccsrc= 'https://launchpad.net/gcc-arm-embedded/5.0/5-2016-q3-update/+download/%s.zip'%(gccarm)
        cpl = '%s/release/download/%s'%(ASROOT,gccarm)
        if(not os.path.exists(cpl)):
            RunCommand('cd %s/release/download && curl -O %s && mkdir -p %s && cd %s && unzip ../%s.zip'%(ASROOT,gccsrc,gccarm,gccarm,gccarm))
        Env.Append(LIBPATH=['%s/lib/gcc/arm-none-eabi/5.4.1'%(cpl)])
        Env.Append(LIBPATH=['%s/arm-none-eabi/lib'%(cpl)])
        Env['CC']='%s/bin/arm-none-eabi-gcc --specs=nosys.specs -std=gnu99'%(cpl)
        Env['CXX']='%s/bin/arm-none-eabi-g++'%(cpl)
        Env['AS']='%s/bin/arm-none-eabi-gcc -c --specs=nosys.specs'%(cpl)
        Env['LINK']='%s/bin/arm-none-eabi-ld'%(cpl)
        Env['S19'] = '%s/bin/%s'%(cpl,Env['S19'])
    else:
        for major in range(10):
            for minor in range(10):
                for patch in range(10):
                    libgcc2 = '/usr/lib/gcc/arm-none-eabi/%s.%s.%s'%(major, minor, patch)
                    if(os.path.exists(libgcc2)):
                        libgcc = libgcc2 # use the latest found version
                        break
        # FIXME to the right path
        Env.Append(LIBPATH=[libgcc,'/usr/lib/arm-none-eabi/newlib'])

def SelectCompilerArm64():
    global Env
    ASROOT = Env['ASROOT']
    if(IsPlatformWindows()):
        gccarm = 'gcc-linaro-7.2.1-2017.11-i686-mingw32_aarch64-elf'
    else:
        gccarm = 'gcc-linaro-7.2.1-2017.11-x86_64_aarch64-elf'
    gccsrc = 'https://releases.linaro.org/components/toolchain/binaries/latest/aarch64-elf/%s.tar.xz'%(gccarm)
    cpl = '%s/release/download/%s'%(ASROOT,gccarm)
    if(not os.path.exists(cpl)):
        RunCommand('cd %s/release/download && curl -O %s && tar xf %s.tar.xz'%(ASROOT,gccsrc,gccarm))
    Env['CC']='%s/bin/aarch64-elf-gcc -std=gnu99 -fno-stack-protector'%(cpl)
    Env['CXX']='%s/bin/aarch64-elf-g++'%(cpl)
    Env['AS']='%s/bin/aarch64-elf-gcc -c'%(cpl)
    Env['LINK']='%s/bin/aarch64-elf-ld'%(cpl)

def SelectCompilerX86():
    global Env
    if(IsPlatformWindows()):
        ASROOT = Env['ASROOT']
        gccx86='i686-elf-tools-windows'
        gccsrc= 'https://github.com/lordmilko/i686-elf-tools/releases/download/7.1.0/i686-elf-tools-windows.zip'
        cpl = '%s/release/download/%s'%(ASROOT,gccx86)
        if(not os.path.exists(cpl)):
            RunCommand('cd %s/release/download && curl -O %s && mkdir -p %s && cd %s && unzip ../%s.zip'%(ASROOT,gccsrc,gccx86,gccx86,gccx86))
        Env['CC']   = '%s/bin/i686-elf-gcc -m32 -std=gnu99 -fno-stack-protector'%(cpl)
        Env['AS']   = '%s/bin/i686-elf-gcc -m32'%(cpl)
        Env['CXX']  = '%s/bin/i686-elf-g++ -m32 -fno-stack-protector'%(cpl)
        Env['LINK'] = '%s/bin/i686-elf-ld -m32 -melf_i386'%(cpl)
        Env.Append(CPPPATH=['%s/lib/gcc/i686-elf/7.1.0/include'%(cpl)])
    else:
        Env['CC']   = 'gcc -m32 -std=gnu99 -fno-stack-protector'
        Env['AS']   = 'gcc -m32'
        Env['CXX']  = 'gcc -m32 -fno-stack-protector'
        Env['LINK'] = 'ld -m32 -melf_i386'

def SelectCompilerCWCC():
    cw = os.getenv('CWCC_PATH')
    if(cw is None):
        cw = 'C:/Program Files (x86)/Freescale/CW for MPC55xx and MPC56xx 2.10'
    if(not os.path.exists(cw)):
        print('==> Please set environment CWCC_PATH\n\tset CWCC_PATH=/path/to/codewarrior_compiler')
        exit()
    cwppc = cw + '/PowerPC_EABI_Tools/Command_Line_Tools'
    Env['CC']   = cwppc + '/mwcceppc.exe'
    Env['AS']   = cwppc + '/mwaseppc.exe'
    Env['CXX']  = cwppc + '/mwcceppc.exe'
    Env['LINK'] = cwppc + '/mwldeppc.exe'
    Env.Append(ASFLAGS=['-gdwarf-2','-vle'])
    Env.Append(CCFLAGS=['-readonlystrings','-RTTI','on','-dialect','c99','-gdwarf-2',
                          '-gccext','on','-gccinc','-cwd','explicit',
                          '-opt','off','-W','most','-abi','eabi',
                          '-fp','soft','-sdata=8','-sdata2=8',
                          '-bool','on','-ipa','off','-inline','smart',
                          '-ppc_asm_to_vle','-vle'])
    Env.Append(CPPPATH=[cw + '/PA_Support/ewl/EWL_C/include',
                          cw + '/PowerPC_EABI_Support/Runtime/Include'])
    Env.Append(LIBPATH=[cw + '/PowerPC_EABI_Support/Runtime/Lib',
                          cw + '/PowerPC_EABI_Support/MSL/MSL_C/PPC_EABI/Lib'])
    Env.Append(LIBS=['Runtime.PPCEABI.VS.UC.a',
                            'MSL_C.PPCEABI.bare.VS.UC.a'])
    Env.Append(CCFLAGS=['-include','asmconfig.h'])
    Env.Append(CPPDEFINES=['USE_FAST_LEAST_TYPE_DEF','VLE_IS_ON=1','_G_va_list=va_list'])

def SelectCompilerPPCEabi():
    # http://download.ronetix.info/toolchains/powerpc/ppc_cross_development_guide.pdf
    cw = os.getenv('PPCGCC_PATH')
    if(cw is None):
        #ppc = 'C:/SysGCC/powerpc-eabi'
        ppc = 'C:/Program Files (x86)/Ronetix/powerpc-eabi'
    if(not os.path.exists(ppc)):
        print('==> Please set environment PPCGCC_PATH\n\tset PPCGCC_PATH=/path/to/powerpc-eabi')
        exit()
    Env['CC']   = ppc + '/bin/powerpc-eabi-gcc.exe'
    Env['AS']   = ppc + '/bin/powerpc-eabi-as.exe'
    Env['CXX']  = ppc + '/bin/powerpc-eabi-g++.exe'
    Env['LINK'] = ppc + '/bin/powerpc-eabi-link.exe'

def BuildingSWCS(swcs):
    for swc in swcs:
        swc = str(swc)
        path = os.path.dirname(swc)
        cmd = 'cd %s && %s %s'%(path, Env['python3'], swc)
        tgt = path+'/Rte_Type.h'
        MKObject([swc], tgt, cmd)

def BuildOFS(ofs):
    for of in ofs:
        src = str(of)
        tgt = src[:-3]+'.h'
        cflags = ''
        for p in Env['CPPPATH']:
            cflags += ' -I%s'%(p)
        for d in Env['CPPDEFINES']:
            cflags += ' -D%s'%(d)
        cmd = 'cp %s .tmp.c && %s -S .tmp.c -o .tmp.S %s'%(src, Env['CC'], cflags)
        cmd += ' && sed -n "/#define/p" .tmp.S > %s'%(tgt)
        MKObject([src], tgt, cmd)

def Building(target, sobjs, env=None):
    if(env is None):
        env = Env
    if(GetOption('splint')):
        splint(objs, env)
    bdir = 'build/%s'%(target)
    objs = []
    xmls = []
    ofs = []
    swcs = []
    arxml= None

    for obj in sobjs:
        if(str(obj)[-6:]=='.arxml'):
            if(arxml is None):
                arxml = obj
            else:
                raise Exception('too much arxml specified! [%s %s]'%(arxml,obj))
        elif(str(obj)[-4:]=='.xml'):
            xmls.append(obj)
        elif(str(obj)[-3:]=='.of'):
            ofs.append(obj)
        elif(str(obj)[-3:]=='.py'):
            swcs.append(obj)
        else:
            objs.append(obj)
    cfgdir = '%s/config'%(bdir)
    AppendPythonPath([cfgdir])
    os.environ['ARXML']=str(arxml)
    cfgdone = '%s/config.done'%(cfgdir)
    if(((not os.path.exists(cfgdone)) and (not GetOption('clean'))) or GetOption('force')):
        MKDir(cfgdir)
        RMFile(cfgdone)
        for xml in xmls:
            MKSymlink(str(xml),'%s/%s'%(cfgdir,os.path.basename(str(xml))))
        MKSymlink(str(arxml),'%s/%s'%(cfgdir,os.path.basename(str(arxml))))
        xcc.XCC(cfgdir,env)
        argen.ArGen.ArGenMain(str(arxml),cfgdir)
        MKFile(cfgdone)
    if('studio' in COMMAND_LINE_TARGETS):
        studio=os.path.abspath('../../com/as.tool/config.infrastructure.system/')
        assert(arxml)
        pd = os.path.abspath(cfgdir)
        RunCommand('cd %s && %s studio.py %s'%(studio,env['python3'],pd))
        exit(0)

    objs += Glob('%s/*.c'%(cfgdir))
    env.Append(CPPPATH=['%s'%(cfgdir)])
    env.Append(ASFLAGS='-I%s'%(cfgdir))
    if('gcc' in env['CC']):
        env.Append(CCFLAGS=['--include','%s/asmconfig.h'%(cfgdir)])

    if(GetOption('clean')):
        RMDir(cfgdir)
        RunCommand('rm -fv *.s19')

    BuildOFS(ofs)
    BuildingSWCS(swcs)
    env.Program(target,objs)

    if(IsPlatformWindows()):target += '.exe'
    #env['POSTACTION'].append('readelf -l %s'%(target))
    for action in env['POSTACTION']:
        env.AddPostAction(target, action)
