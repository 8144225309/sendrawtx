#!/bin/bash
# Build all 3 entry points as self-contained HTML files
set -e

echo "Building index.html..."
ENTRY=index npx vite build
echo ""

echo "Building broadcast.html..."
ENTRY=broadcast npx vite build
echo ""

echo "Building result.html..."
ENTRY=result npx vite build
echo ""

echo "Build complete! Files in dist/"
ls -lh dist/*.html
