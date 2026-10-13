#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Private application's actual request/shader dialect: 4.3/430 -> 4.2/420.
No GL driver version, capabilities, entry points or errors are overridden.
"""
import hashlib,struct,re
SOURCE_SHA='9c4406a75563a32d76153bd438106a84ef1af94d91339e79628b5080eed74a50'
def patch(source):
 if hashlib.sha256(source).hexdigest()!=SOURCE_SHA:raise ValueError('source SHA mismatch')
 if source[:6]!=b'\x7fELF\x02\x01' or struct.unpack_from('<H',source,18)[0]!=183:raise ValueError('ELF architecture')
 phoff=struct.unpack_from('<Q',source,32)[0];size,count=struct.unpack_from('<HH',source,54)
 def offset(va):
  hits=[]
  for i in range(count):
   kind,flags,off,addr,phys,filesz,memsz,align=struct.unpack_from('<IIQQQQQQ',source,phoff+i*size)
   if kind==1 and addr<=va<addr+filesz:hits.append(off+va-addr)
  if len(hits)!=1:raise ValueError('ambiguous VA')
  return hits[0]
 o=offset(0x1f8a28)
 if source[o:o+4]!=struct.pack('<I',0x52800062):raise ValueError('minor-version instruction')
 output=bytearray(source);output[o:o+4]=struct.pack('<I',0x52800042)
 hits=list(re.finditer(b'#version 430',source))
 if [m.start() for m in hits]!=[0x795f93,0x79609b,0x796a3b,0x796afb]:raise ValueError('shader source drift')
 for m in hits:output[m.start()+10]=ord('2')
 changed=[i for i,(a,b) in enumerate(zip(source,output)) if a!=b]
 expected=sorted([o]+[m.start()+10 for m in hits])
 if changed!=expected or len(source)!=len(output):raise ValueError('unexpected binary changes')
 return bytes(output),dict(source_sha=SOURCE_SHA,result_sha=hashlib.sha256(output).hexdigest(),changed_offsets=changed,actual_request='OpenGL core 4.2',actual_shader_dialect='GLSL 420',driver_version_override=False)


if __name__ == '__main__':
 import argparse,os
 from pathlib import Path
 ap=argparse.ArgumentParser(description='Patch an exact user-supplied CEF copy locally; never install or download Steam.')
 ap.add_argument('--input',type=Path,required=True)
 ap.add_argument('--output',type=Path,required=True)
 args=ap.parse_args()
 if args.input.resolve()==args.output.resolve():raise SystemExit('Refusing in-place change')
 result,receipt=patch(args.input.read_bytes())
 if receipt['result_sha']!='9250a41b4eb9f4446e018868a8c8c9b4fbb5240164c704e2e1b7e57d99df1400':raise SystemExit('Output hash differs')
 with args.output.open('xb') as f:f.write(result);f.flush();os.fsync(f.fileno())
 args.output.chmod(0o755)
 print(receipt)
