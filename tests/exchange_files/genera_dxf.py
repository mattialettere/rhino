# Genera i DXF di prova (con ezdxf, libreria indipendente da Kurva3D) per tests/exchange_files.
import sys, math, ezdxf
from ezdxf.enums import TextEntityAlignment
out = sys.argv[1]

def build(version):
    doc = ezdxf.new(version, setup=True, units=4)  # mm
    doc.header['$MEASUREMENT'] = 1
    doc.linetypes  # DASHED, CENTER... da setup
    L = doc.layers
    L.add('MURI', color=1, linetype='DASHED', lineweight=50)
    L.add('SPENTO', color=3).off()
    lk = L.add('BLOCCATO', color=5); lk.lock()
    tc = L.add('VERO_COLORE'); tc.rgb = (12, 34, 56)
    L.add('QUOTE', color=2)
    L.add('TESTI', color=4)
    L.add('RETINI', color=6)
    msp = doc.modelspace()
    # geometria con attributi per oggetto
    msp.add_line((0, 0), (100, 0), dxfattribs={'layer': 'MURI'})
    msp.add_line((0, 10), (100, 10), dxfattribs={'layer': 'MURI', 'color': 3, 'linetype': 'CENTER', 'lineweight': 35})
    if version != 'R12':
        msp.add_line((0, 20), (100, 20), dxfattribs={'layer': 'VERO_COLORE', 'true_color': ezdxf.colors.rgb2int((200, 100, 50))})
    msp.add_circle((50, 50), 10, dxfattribs={'layer': 'SPENTO'})
    msp.add_arc((150, 50), 10, 0, 90, dxfattribs={'layer': 'BLOCCATO'})
    if version != 'R12':
        msp.add_ellipse((200, 50), major_axis=(20, 0), ratio=0.5, start_param=0, end_param=math.pi)
        msp.add_spline([(0, 100), (10, 120), (30, 90), (50, 110)])
        msp.add_lwpolyline([(0, 200, 0, 0, 1), (20, 200), (20, 220)], format='xyseb', close=True)
    else:
        msp.add_polyline2d([(0, 200), (20, 200), (20, 220)], close=True)
    # testi: accenti e simboli
    msp.add_text('Perché è così: 30°', height=5, dxfattribs={'layer': 'TESTI'}).set_placement((0, 300))
    msp.add_text('CENTRATO', height=4, rotation=90, dxfattribs={'layer': 'TESTI'}).set_placement((100, 300), align=TextEntityAlignment.MIDDLE_CENTER)
    if version != 'R12':
        msp.add_mtext('Riga uno\\PRiga {\\fArial|b1;due} %%c12', dxfattribs={'layer': 'TESTI', 'char_height': 3, 'insert': (0, 350), 'attachment_point': 1})
    # blocchi annidati con attributi
    vite = doc.blocks.new('VITE', base_point=(5, 0))
    vite.add_circle((5, 0), 2)  # livello 0, colore DaLayer -> DaBlocco
    vite.add_line((3, 0), (7, 0), dxfattribs={'color': 0})  # DaBlocco
    piastra = doc.blocks.new('PIASTRA')
    piastra.add_lwpolyline([(0, 0), (40, 0), (40, 20), (0, 20)], close=True, dxfattribs={'layer': 'MURI'}) if version != 'R12' else piastra.add_polyline2d([(0, 0), (40, 0), (40, 20), (0, 20)], close=True, dxfattribs={'layer': 'MURI'})
    piastra.add_blockref('VITE', (10, 10))
    piastra.add_blockref('VITE', (30, 10), dxfattribs={'rotation': 90})
    piastra.add_attdef('CODICE', (0, -5), dxfattribs={'height': 2})
    ref = msp.add_blockref('PIASTRA', (300, 0), dxfattribs={'layer': 'BLOCCATO', 'xscale': 2, 'yscale': 2, 'rotation': 30, 'color': 1})
    ref.add_attrib('CODICE', 'P-001', (300, -10), dxfattribs={'height': 2})
    msp.add_blockref('PIASTRA', (400, 0))
    # quote
    if version != 'R12':
        st = doc.dimstyles.get('Standard') if 'Standard' in doc.dimstyles else doc.dimstyles.new('Standard')
        msp.add_linear_dim(base=(0, -20), p1=(0, 0), p2=(100, 0), dxfattribs={'layer': 'QUOTE'}).render()
        msp.add_aligned_dim(p1=(0, 0), p2=(30, 40), distance=10, dxfattribs={'layer': 'QUOTE'}).render()
        msp.add_radius_dim(center=(50, 50), radius=10, angle=45, dxfattribs={'layer': 'QUOTE'}).render()
        msp.add_diameter_dim(center=(50, 50), radius=10, angle=135, dxfattribs={'layer': 'QUOTE'}).render()
        msp.add_angular_dim_3p(base=(170, 70), center=(150, 50), p1=(160, 50), p2=(150, 60), dxfattribs={'layer': 'QUOTE'}).render()
        msp.add_linear_dim(base=(0, -40), p1=(0, 0), p2=(100, 0), text='LUNGHEZZA <>', dxfattribs={'layer': 'QUOTE'}).render()
        # retini: pieno e a motivo con isola
        h = msp.add_hatch(color=2, dxfattribs={'layer': 'RETINI'})
        h.paths.add_polyline_path([(500, 0), (560, 0), (560, 40), (500, 40)], is_closed=True)
        h2 = msp.add_hatch(dxfattribs={'layer': 'RETINI'})
        h2.set_pattern_fill('ANSI31', scale=2, angle=0)
        h2.paths.add_polyline_path([(600, 0), (700, 0), (700, 60), (600, 60)], is_closed=True, flags=1)
        h2.paths.add_polyline_path([(620, 20), (640, 20), (640, 40), (620, 40)], is_closed=True, flags=16)
        ep = h2.paths.add_edge_path(flags=16)
        ep.add_arc((670, 30), 8, 0, 360)
        # direttrice
        msp.add_leader([(0, 400), (20, 420), (40, 420)], dxfattribs={'layer': 'TESTI'})
        # gruppo
        g = doc.groups.new('COPPIA')
        a = msp.add_line((0, 500), (10, 500))
        b = msp.add_line((0, 510), (10, 510))
        g.extend([a, b])
    # 3DFACE e polyface
    msp.add_3dface([(0, 600, 0), (10, 600, 0), (10, 610, 5), (0, 610, 5)])
    pf = msp.add_polyface()
    pf.append_face([(0, 700, 0), (10, 700, 0), (10, 710, 0), (0, 710, 0)])
    pf.append_face([(0, 700, 0), (10, 700, 0), (5, 705, 10)])
    # spazio carta
    if version != 'R12':
        psp = doc.paperspace()
        psp.add_text('CARTIGLIO', height=5).set_placement((10, 10))
        psp.add_line((5, 5), (200, 5))
    return doc

d = build('R2018'); d.saveas(out + '/completo_r2018.dxf')
d.saveas(out + '/completo_r2018_binario.dxf', fmt='bin')
d = build('R12'); d.saveas(out + '/completo_r12.dxf', encoding='cp1252')
# unita' in metri
d = ezdxf.new('R2010', units=6); d.modelspace().add_line((0, 0), (1, 0)); d.saveas(out + '/metri.dxf')
print('ok')
