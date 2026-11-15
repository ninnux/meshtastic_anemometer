"""Massa e momento d'inerzia attorno all'asse Z di un Manifold (pieno, densità uniforme)."""
import numpy as np

RHO_PETG = 1.27e-3  # g/mm3


def massa_inerzia(m, rho=RHO_PETG):
    mesh = m.to_mesh()
    v = np.asarray(mesh.vert_properties)[:, :3]
    f = np.asarray(mesh.tri_verts)
    a, b, c = v[f[:, 0]], v[f[:, 1]], v[f[:, 2]]
    vol = np.einsum("ij,ij->i", a, np.cross(b, c)) / 6.0
    def second(i):
        ai, bi, ci = a[:, i], b[:, i], c[:, i]
        return vol / 10.0 * (ai**2 + bi**2 + ci**2 + ai*bi + ai*ci + bi*ci)
    iz = (second(0) + second(1)).sum()
    return vol.sum() * rho, iz * rho  # g, g*mm2
