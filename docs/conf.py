# Configuration file for the Sphinx documentation builder.

import os
import sys
from pathlib import Path

_repo_root = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(_repo_root / ".github" / "scripts"))

from version import resolve_version

# -- Project information -----------------------------------------------------
project = 'LanceDB C API'
copyright = '2025, The LanceDB Authors'
author = 'The LanceDB Authors'
release = resolve_version(_repo_root)

# -- General configuration ---------------------------------------------------
extensions = [
    'breathe',
    'sphinx.ext.autodoc',
    'sphinx.ext.intersphinx',
    'sphinx.ext.viewcode',
]

# Breathe Configuration
# When built through CMake, XML files are in the build directory
# When built manually, they are in the parent directory
xml_dir = os.environ.get('DOXYGEN_XML_DIR', '../xml')
breathe_projects = {
    "LanceDB": xml_dir
}
breathe_default_project = "LanceDB"

templates_path = ['_templates']
exclude_patterns = ['_build', 'Thumbs.db', '.DS_Store']

# -- Options for HTML output -------------------------------------------------
html_theme = 'sphinx_rtd_theme'
html_static_path = ['_static']

# Theme options
html_theme_options = {
    'navigation_depth': 4,
    'collapse_navigation': False,
    'sticky_navigation': True,
    'includehidden': True,
}

# -- Options for intersphinx extension ---------------------------------------
intersphinx_mapping = {
    'python': ('https://docs.python.org/3', None),
}
