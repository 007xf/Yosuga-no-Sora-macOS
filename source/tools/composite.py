"""composite.py <gl.png> <ui.png> <out.png> - overlay the native UI snapshot on the GL frame."""
import sys
from PIL import Image
gl = Image.open(sys.argv[1]).convert('RGBA')
ui = Image.open(sys.argv[2]).convert('RGBA')
# The UI snapshot covers the content view (same size as the GL drawable).
ui = ui.resize(gl.size)
out = Image.alpha_composite(gl, ui)
out.convert('RGB').resize((gl.width // 2, gl.height // 2)).save(sys.argv[3])
