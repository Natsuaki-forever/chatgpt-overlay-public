"""Copy runtime notices verbatim; never fetch dependencies during a build."""
import argparse
from importlib.metadata import PackageNotFoundError, distribution, distributions
import json
from pathlib import Path
import shutil
import sys


ROOT=Path(__file__).resolve().parents[1]


def collect(destination,vision_deps=None,native_editor=False):
    if native_editor and not vision_deps:
        return
    destination=Path(destination)/'runtime'
    destination.mkdir(parents=True,exist_ok=True)
    if not native_editor:
        shutil.copytree(ROOT/'third_party/runtime-licenses',destination/'qt-license-texts',dirs_exist_ok=True)
    shutil.copyfile(Path(sys.base_prefix)/'LICENSE.txt',destination/'Python-LICENSE.txt')
    versions={'Python':sys.version.split()[0]}
    runtime_packages=('PyInstaller',) if native_editor else ('PySide6_Essentials','shiboken6','PyInstaller')
    packages=[(name,distribution(name)) for name in runtime_packages]
    if vision_deps:
        packages.append(('numpy',distribution('numpy')))
        local=list(distributions(path=[str(Path(vision_deps).resolve(strict=True))]))
        opencv=[p for p in local
                if p.metadata['Name'].lower().replace('_','-')=='opencv-python-headless']
        if len(opencv)!=1:raise RuntimeError('Exactly one headless OpenCV distribution is required')
        packages.append(('opencv-python-headless',opencv[0]))
        # NumPy's optional helper modules can be included by the freezer even
        # though the live worker does not load them. Ship their notices too.
        for name in ('psutil','charset-normalizer','threadpoolctl','typing-extensions','PyYAML'):
            matches=[p for p in local if p.metadata['Name'].lower().replace('_','-')==name.lower()]
            if matches: packages.append((name,matches[0]))
            else:
                try: packages.append((name,distribution(name)))
                except PackageNotFoundError: pass
    for name,package in packages:
        versions[name]=package.version
        folder=destination/name
        folder.mkdir(exist_ok=True)
        metadata=next(file for file in package.files if file.name=='METADATA')
        shutil.copyfile(package.locate_file(metadata),folder/'METADATA.txt')
        for file in package.files:
            parts=file.parts
            if 'licenses' in parts and Path(package.locate_file(file)).is_file():
                relative=Path(*parts[parts.index('licenses')+1:])
                target=folder/relative
                target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(package.locate_file(file),target)
            elif any(marker in file.name.upper() for marker in ('LICENSE','COPYING','NOTICE')) and Path(package.locate_file(file)).is_file():
                target=folder/Path(*parts);target.parent.mkdir(parents=True,exist_ok=True)
                shutil.copyfile(package.locate_file(file),target)
    (destination/'versions.json').write_text(json.dumps(versions,indent=2),encoding='utf-8')


if __name__=='__main__':
    parser=argparse.ArgumentParser()
    parser.add_argument('--destination',required=True)
    parser.add_argument('--vision-deps')
    parser.add_argument('--native-editor',action='store_true')
    args=parser.parse_args();collect(args.destination,args.vision_deps,args.native_editor)

