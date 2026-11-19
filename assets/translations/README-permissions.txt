The console app runs as a non-root user. Catalog files must be world-readable (0644):
a catalog only root can read is reported by the loader as missing, and the UI stays English.
