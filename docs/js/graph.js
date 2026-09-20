// sandfleaOS Subsystem Matrix & Tech-Tree Graph Engine
// Minimalist Brutalist / Monochrome Theme (Black, White, 1px sharp borders, Top Color Bars)
// Advanced Sugiyama-style Layered DAG Layout Engine with Crisp High-DPI Text & Hover Wires

(function () {
  'use strict';

  let data = null;
  const canvas = document.getElementById('graph-canvas');
  const ctx = canvas.getContext('2d');
  const minimapCanvas = document.getElementById('minimap-canvas');
  const minimapCtx = minimapCanvas.getContext('2d');
  const viewport = document.getElementById('viewport-container');
  const inspector = document.getElementById('side-inspector');

  // Sharp Rectangular Node Dimensions (0px radius)
  const NODE_WIDTH = 220;
  const NODE_HEIGHT = 96;

  // Viewport State
  const camera = {
    x: 0,
    y: 0,
    zoom: 0.85,
    minZoom: 0.2,
    maxZoom: 2.2
  };

  let isPanning = false;
  let startPanX = 0;
  let startPanY = 0;

  // Interaction State
  let selectedNode = null;
  let hoveredNode = null;
  let draggedNode = null;
  let dragOffsetX = 0;
  let dragOffsetY = 0;
  let isDraggingNode = false;
  let activeFilterCategory = 'all';
  let searchQuery = '';

  // Cached Dependency Graph
  const nodeMap = new Map();
  const categoryMap = new Map();
  let upstreamSet = new Set();
  let downstreamSet = new Set();

  // Animation Frame
  let animTime = 0;
  let needsRedraw = true;

  // -------------------------------------------------------------
  // Initialization & Data Loading
  // -------------------------------------------------------------
  async function init() {
    setupCanvasDPI();
    window.addEventListener('resize', () => {
      setupCanvasDPI();
      requestRedraw();
    });

    try {
      const resp = await fetch('data/roadmap.json');
      if (resp.ok) {
        data = await resp.json();
      } else {
        throw new Error('Fetch status ' + resp.status);
      }
    } catch (e) {
      if (window.ROADMAP_DATA) {
        data = window.ROADMAP_DATA;
      } else {
        console.error('Failed to load roadmap dataset:', e);
        return;
      }
    }

    processData();
    setupUI();
    setupEvents();
    
    // Auto layout and center by default
    autoLayoutHierarchy();

    startAnimationLoop();
  }

  function processData() {
    nodeMap.clear();
    categoryMap.clear();

    data.categories.forEach(cat => categoryMap.set(cat.id, cat));

    data.nodes.forEach(node => {
      if (!node.position) node.position = { x: 0, y: 0 };
      if (!node.dependsOn) node.dependsOn = [];
      if (!node.unlocks) node.unlocks = [];
      nodeMap.set(node.id, node);
    });

    // Populate unlocks (dependents) automatically from dependsOn
    data.nodes.forEach(node => {
      node.dependsOn.forEach(depId => {
        const parent = nodeMap.get(depId);
        if (parent && !parent.unlocks.includes(node.id)) {
          parent.unlocks.push(node.id);
        }
      });
    });

    updateStats();
  }

  function setupCanvasDPI() {
    // Force at least 2x supersampling to eliminate zoom-out text blur
    const dpr = Math.max(window.devicePixelRatio || 1, 2);
    const rect = viewport.getBoundingClientRect();
    
    canvas.width = Math.round(rect.width * dpr);
    canvas.height = Math.round(rect.height * dpr);
    canvas.style.width = `${rect.width}px`;
    canvas.style.height = `${rect.height}px`;
    
    ctx.resetTransform();
    ctx.scale(dpr, dpr);
    ctx.imageSmoothingEnabled = true;
    ctx.imageSmoothingQuality = 'high';

    minimapCanvas.width = 170 * dpr;
    minimapCanvas.height = 110 * dpr;
    minimapCanvas.style.width = '170px';
    minimapCanvas.style.height = '110px';
    minimapCtx.resetTransform();
    minimapCtx.scale(dpr, dpr);
  }

  function requestRedraw() {
    needsRedraw = true;
  }

  // -------------------------------------------------------------
  // UI & Stats Setup
  // -------------------------------------------------------------
  function updateStats() {
    let completed = 0;
    let inProgress = 0;
    let planned = 0;
    let research = 0;

    data.nodes.forEach(n => {
      if (n.status === 'completed') completed++;
      else if (n.status === 'in-progress') inProgress++;
      else if (n.status === 'planned') planned++;
      else if (n.status === 'research') research++;
    });

    document.getElementById('stat-completed-count').textContent = `${completed} STABLE`;
    document.getElementById('stat-inprogress-count').textContent = `${inProgress} ACTIVE`;
    document.getElementById('stat-planned-count').textContent = `${planned + research} PLANNED`;

    const filterContainer = document.getElementById('category-filters');
    filterContainer.innerHTML = '';

    const allChip = document.createElement('div');
    allChip.className = 'filter-chip active';
    allChip.dataset.cat = 'all';
    allChip.innerHTML = `<span>ALL LAYERS (${data.nodes.length})</span>`;
    allChip.addEventListener('click', () => setFilter('all'));
    filterContainer.appendChild(allChip);

    data.categories.forEach(cat => {
      const count = data.nodes.filter(n => n.category === cat.id).length;
      if (count === 0) return;
      const chip = document.createElement('div');
      chip.className = 'filter-chip';
      chip.dataset.cat = cat.id;
      chip.innerHTML = `<span class="chip-dot" style="background: ${cat.color}"></span><span>${cat.name} (${count})</span>`;
      chip.addEventListener('click', () => setFilter(cat.id));
      filterContainer.appendChild(chip);
    });
  }

  function setFilter(catId) {
    activeFilterCategory = catId;
    document.querySelectorAll('.filter-chip').forEach(el => {
      el.classList.toggle('active', el.dataset.cat === catId);
    });
    requestRedraw();
  }

  function setupUI() {
    const searchInput = document.getElementById('search-input');
    searchInput.addEventListener('input', (e) => {
      searchQuery = e.target.value.toLowerCase().trim();
      if (searchQuery) {
        const match = data.nodes.find(n =>
          n.title.toLowerCase().includes(searchQuery) ||
          n.id.toLowerCase().includes(searchQuery) ||
          n.summary.toLowerCase().includes(searchQuery)
        );
        if (match) {
          focusNode(match);
        }
      }
      requestRedraw();
    });

    document.getElementById('btn-zoom-in').addEventListener('click', () => zoomBy(1.2));
    document.getElementById('btn-zoom-out').addEventListener('click', () => zoomBy(0.8));
    document.getElementById('btn-fit-all').addEventListener('click', centerGraph);
    document.getElementById('btn-auto-layout').addEventListener('click', autoLayoutHierarchy);

    document.getElementById('panel-close-btn').addEventListener('click', () => {
      deselectNode();
    });

    document.getElementById('btn-clear-highlight').addEventListener('click', () => {
      deselectNode();
    });
  }

  // -------------------------------------------------------------
  // Graph Traversal & Path Highlighting
  // -------------------------------------------------------------
  function calculateDependencies(node) {
    upstreamSet.clear();
    downstreamSet.clear();

    if (!node) return;

    const traverseUp = (id) => {
      const n = nodeMap.get(id);
      if (!n || !n.dependsOn) return;
      n.dependsOn.forEach(depId => {
        if (!upstreamSet.has(depId)) {
          upstreamSet.add(depId);
          traverseUp(depId);
        }
      });
    };
    traverseUp(node.id);

    const traverseDown = (id) => {
      const n = nodeMap.get(id);
      if (!n || !n.unlocks) return;
      n.unlocks.forEach(childId => {
        if (!downstreamSet.has(childId)) {
          downstreamSet.add(childId);
          traverseDown(childId);
        }
      });
    };
    traverseDown(node.id);
  }

  function selectNode(node) {
    selectedNode = node;
    calculateDependencies(node);
    openInspector(node);

    const pill = document.getElementById('highlight-pill');
    const pillText = document.getElementById('highlight-node-title');
    pill.classList.add('visible');
    pillText.textContent = `${node.title} (${upstreamSet.size} PREREQ, ${downstreamSet.size} UNLOCKS)`;

    requestRedraw();
  }

  function deselectNode() {
    selectedNode = null;
    upstreamSet.clear();
    downstreamSet.clear();
    closeInspector();
    document.getElementById('highlight-pill').classList.remove('visible');
    requestRedraw();
  }

  function focusNode(node) {
    selectNode(node);
    const rect = viewport.getBoundingClientRect();
    const targetX = rect.width / 2 - (node.position.x + NODE_WIDTH / 2) * camera.zoom;
    const targetY = rect.height / 2 - (node.position.y + NODE_HEIGHT / 2) * camera.zoom;

    smoothPanTo(targetX, targetY);
  }

  function smoothPanTo(targetX, targetY) {
    const startX = camera.x;
    const startY = camera.y;
    const startTime = performance.now();
    const duration = 300;

    function step(now) {
      const progress = Math.min(1, (now - startTime) / duration);
      const ease = 1 - Math.pow(1 - progress, 3);
      camera.x = startX + (targetX - startX) * ease;
      camera.y = startY + (targetY - startY) * ease;
      requestRedraw();
      if (progress < 1) {
        requestAnimationFrame(step);
      }
    }
    requestAnimationFrame(step);
  }

  // -------------------------------------------------------------
  // Side Inspector Panel
  // -------------------------------------------------------------
  function openInspector(node) {
    const cat = categoryMap.get(node.category) || { name: node.category, color: '#ffffff' };
    const catBadge = document.getElementById('panel-cat-badge');
    catBadge.textContent = cat.name;

    const statusBadge = document.getElementById('panel-status-badge');
    statusBadge.className = `panel-status-badge ${node.status}`;
    const statusLabels = {
      'completed': 'STABLE',
      'in-progress': 'IN PROGRESS',
      'planned': 'PLANNED',
      'research': 'RESEARCH'
    };
    statusBadge.textContent = statusLabels[node.status] || node.status.toUpperCase();

    document.getElementById('panel-title').textContent = node.title;
    document.getElementById('panel-summary').textContent = node.summary;

    document.getElementById('panel-desc').textContent = node.details?.description || node.summary;
    document.getElementById('panel-importance').textContent = node.details?.importance || 'Essential OS component.';

    document.getElementById('panel-notes').textContent = node.details?.notes || 'No extra notes recorded.';

    const diffList = document.getElementById('panel-difficulties');
    diffList.innerHTML = '';
    (node.details?.difficulties || ['No major blockers noted.']).forEach(item => {
      const li = document.createElement('li');
      li.textContent = item;
      diffList.appendChild(li);
    });

    const winsList = document.getElementById('panel-wins');
    winsList.innerHTML = '';
    (node.details?.easyParts || ['Standard implementation patterns.']).forEach(item => {
      const li = document.createElement('li');
      li.textContent = item;
      winsList.appendChild(li);
    });

    const filesContainer = document.getElementById('panel-files');
    filesContainer.innerHTML = '';
    if (node.details?.sourceFiles && node.details.sourceFiles.length > 0) {
      node.details.sourceFiles.forEach(file => {
        const item = document.createElement('div');
        item.className = 'file-item';
        item.innerHTML = `
          <div class="file-path">${file.path || file.todo}</div>
          <div class="file-desc">${file.description || ''}</div>
        `;
        filesContainer.appendChild(item);
      });
    } else {
      filesContainer.innerHTML = '<div style="font-size:11px; color:var(--text-secondary); font-family:var(--font-mono);">No source files mapped yet.</div>';
    }

    const tasksContainer = document.getElementById('panel-subtasks');
    tasksContainer.innerHTML = '';
    if (node.details?.subtasks && node.details.subtasks.length > 0) {
      node.details.subtasks.forEach(task => {
        const item = document.createElement('div');
        item.className = `subtask-item ${task.done ? 'done' : ''}`;
        item.innerHTML = `
          <div class="subtask-checkbox ${task.done ? 'done' : ''}">${task.done ? '✓' : ''}</div>
          <span>${task.text}</span>
        `;
        tasksContainer.appendChild(item);
      });
    } else {
      tasksContainer.innerHTML = '<div style="font-size:11px; color:var(--text-secondary); font-family:var(--font-mono);">No subtasks defined.</div>';
    }

    const prereqsContainer = document.getElementById('panel-prereqs');
    prereqsContainer.innerHTML = '';
    if (node.dependsOn.length > 0) {
      node.dependsOn.forEach(depId => {
        const dep = nodeMap.get(depId);
        if (!dep) return;
        const chip = document.createElement('div');
        chip.className = 'dep-chip';
        chip.innerHTML = `<span class="chip-arrow"><</span><span>${dep.title}</span>`;
        chip.addEventListener('click', () => focusNode(dep));
        prereqsContainer.appendChild(chip);
      });
    } else {
      prereqsContainer.innerHTML = '<span style="font-size:11px; color:var(--text-secondary); font-family:var(--font-mono);">NONE (ROOT)</span>';
    }

    const unlocksContainer = document.getElementById('panel-unlocks');
    unlocksContainer.innerHTML = '';
    if (node.unlocks.length > 0) {
      node.unlocks.forEach(childId => {
        const child = nodeMap.get(childId);
        if (!child) return;
        const chip = document.createElement('div');
        chip.className = 'dep-chip';
        chip.innerHTML = `<span>${child.title}</span><span class="chip-arrow">></span>`;
        chip.addEventListener('click', () => focusNode(child));
        unlocksContainer.appendChild(chip);
      });
    } else {
      unlocksContainer.innerHTML = '<span style="font-size:11px; color:var(--text-secondary); font-family:var(--font-mono);">NONE (LEAF)</span>';
    }

    inspector.classList.add('open');
  }

  function closeInspector() {
    inspector.classList.remove('open');
  }

  // -------------------------------------------------------------
  // Navigation & Zoom Helpers
  // -------------------------------------------------------------
  function centerGraph() {
    if (!data || data.nodes.length === 0) return;

    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    data.nodes.forEach(n => {
      minX = Math.min(minX, n.position.x);
      minY = Math.min(minY, n.position.y);
      maxX = Math.max(maxX, n.position.x + NODE_WIDTH);
      maxY = Math.max(maxY, n.position.y + NODE_HEIGHT);
    });

    const rect = viewport.getBoundingClientRect();
    const graphW = maxX - minX + 160;
    const graphH = maxY - minY + 160;

    const zoomX = rect.width / graphW;
    const zoomY = rect.height / graphH;
    camera.zoom = Math.min(Math.max(Math.min(zoomX, zoomY), camera.minZoom), 0.95);

    camera.x = (rect.width - (maxX + minX) * camera.zoom) / 2;
    camera.y = (rect.height - (maxY + minY) * camera.zoom) / 2;

    requestRedraw();
  }

  function zoomBy(factor) {
    const rect = viewport.getBoundingClientRect();
    const centerX = rect.width / 2;
    const centerY = rect.height / 2;

    const newZoom = Math.min(Math.max(camera.zoom * factor, camera.minZoom), camera.maxZoom);
    camera.x = centerX - (centerX - camera.x) * (newZoom / camera.zoom);
    camera.y = centerY - (centerY - camera.y) * (newZoom / camera.zoom);
    camera.zoom = newZoom;

    requestRedraw();
  }

  // -------------------------------------------------------------
  // Sugiyama Layered DAG Layout Engine
  // -------------------------------------------------------------
  function autoLayoutHierarchy() {
    if (!data || data.nodes.length === 0) return;

    const rankMap = new Map();
    const memo = new Map();

    function computeRank(id, visited = new Set()) {
      if (memo.has(id)) return memo.get(id);
      if (visited.has(id)) return 0;

      visited.add(id);
      const n = nodeMap.get(id);
      if (!n || n.dependsOn.length === 0) {
        memo.set(id, 0);
        return 0;
      }

      let maxRank = 0;
      n.dependsOn.forEach(pId => {
        maxRank = Math.max(maxRank, computeRank(pId, new Set(visited)) + 1);
      });

      memo.set(id, maxRank);
      return maxRank;
    }

    data.nodes.forEach(n => {
      rankMap.set(n.id, computeRank(n.id));
    });

    const maxLayer = Math.max(...Array.from(rankMap.values()), 0);
    const layers = Array.from({ length: maxLayer + 1 }, () => []);

    data.nodes.forEach(n => {
      const r = rankMap.get(n.id) || 0;
      layers[r].push(n);
    });

    const categoryOrder = {
      'arch_boot': 0,
      'memory': 1,
      'io_drivers': 2,
      'storage_fs': 3,
      'concurrency_ipc': 4,
      'graphics_ui': 5,
      'runtime_wasm': 6,
      'apps_userland': 7,
      'roadmap_future': 8
    };

    layers.forEach(layer => {
      layer.sort((a, b) => {
        const catA = categoryOrder[a.category] ?? 99;
        const catB = categoryOrder[b.category] ?? 99;
        return catA - catB;
      });
    });

    const orderIndexMap = new Map();

    function updateOrderIndices() {
      layers.forEach(layer => {
        layer.forEach((node, idx) => {
          orderIndexMap.set(node.id, idx);
        });
      });
    }
    updateOrderIndices();

    for (let sweep = 0; sweep < 3; sweep++) {
      for (let l = 1; l <= maxLayer; l++) {
        const layer = layers[l];
        layer.forEach(node => {
          if (node.dependsOn.length > 0) {
            let sum = 0;
            let count = 0;
            node.dependsOn.forEach(pId => {
              if (orderIndexMap.has(pId)) {
                sum += orderIndexMap.get(pId);
                count++;
              }
            });
            node._bary = count > 0 ? sum / count : (orderIndexMap.get(node.id) || 0);
          } else {
            node._bary = orderIndexMap.get(node.id) || 0;
          }
        });
        layer.sort((a, b) => a._bary - b._bary);
        updateOrderIndices();
      }

      for (let l = maxLayer - 1; l >= 0; l--) {
        const layer = layers[l];
        layer.forEach(node => {
          if (node.unlocks.length > 0) {
            let sum = 0;
            let count = 0;
            node.unlocks.forEach(cId => {
              if (orderIndexMap.has(cId)) {
                sum += orderIndexMap.get(cId);
                count++;
              }
            });
            node._bary = count > 0 ? sum / count : (orderIndexMap.get(node.id) || 0);
          } else {
            node._bary = orderIndexMap.get(node.id) || 0;
          }
        });
        layer.sort((a, b) => a._bary - b._bary);
        updateOrderIndices();
      }
    }

    // Doubled spacing for clear breathing room and uncluttered curves
    const COL_GAP = 460;
    const ROW_GAP = 180;

    layers.forEach((layer, colIdx) => {
      const colHeight = (layer.length - 1) * ROW_GAP;
      const startY = -colHeight / 2 + 250;
      const startX = 80 + colIdx * COL_GAP;

      layer.forEach((node, rowIdx) => {
        node.position.x = startX;
        node.position.y = startY + rowIdx * ROW_GAP;
      });
    });

    centerGraph();
  }

  // -------------------------------------------------------------
  // Canvas Rendering Pipeline
  // -------------------------------------------------------------
  function startAnimationLoop() {
    function loop(time) {
      animTime = time * 0.001;
      draw();
      requestAnimationFrame(loop);
    }
    requestAnimationFrame(loop);
  }

  function draw() {
    const rect = viewport.getBoundingClientRect();
    ctx.clearRect(0, 0, rect.width, rect.height);

    ctx.save();
    ctx.translate(camera.x, camera.y);
    ctx.scale(camera.zoom, camera.zoom);

    // 1. Draw Clean Dependency Edges (with hover and selection highlight)
    drawEdges();

    // 2. Draw Nodes (with crisp font rendering)
    drawNodes();

    ctx.restore();

    // 3. Draw MiniMap
    drawMinimap();
  }

  function drawEdges() {
    ctx.save();

    data.nodes.forEach(node => {
      if (!node.dependsOn || node.dependsOn.length === 0) return;

      node.dependsOn.forEach(parentId => {
        const parent = nodeMap.get(parentId);
        if (!parent) return;

        // Selection path states
        const isUpstream = selectedNode && (
          (node.id === selectedNode.id && upstreamSet.has(parent.id)) ||
          (upstreamSet.has(node.id) && upstreamSet.has(parent.id))
        );

        const isDownstream = selectedNode && (
          (parent.id === selectedNode.id && downstreamSet.has(node.id)) ||
          (downstreamSet.has(parent.id) && downstreamSet.has(node.id))
        );

        const isDirectToSelected = selectedNode && (
          parent.id === selectedNode.id || node.id === selectedNode.id
        );

        // Hover wire detection (input wire into hoveredNode OR output wire from hoveredNode)
        const isHoverConnected = hoveredNode && (
          parent.id === hoveredNode.id || node.id === hoveredNode.id
        );

        const isFaded = selectedNode && !isUpstream && !isDownstream && !isDirectToSelected && !isHoverConnected;

        const x1 = parent.position.x + NODE_WIDTH;
        const y1 = parent.position.y + NODE_HEIGHT / 2;
        const x2 = node.position.x;
        const y2 = node.position.y + NODE_HEIGHT / 2;

        const dx = Math.max(Math.abs(x2 - x1) * 0.45, 30);
        const cp1x = x1 + dx;
        const cp1y = y1;
        const cp2x = x2 - dx;
        const cp2y = y2;

        ctx.beginPath();
        ctx.moveTo(x1, y1);
        ctx.bezierCurveTo(cp1x, cp1y, cp2x, cp2y, x2, y2);

        if (isHoverConnected) {
          ctx.strokeStyle = '#ffffff';
          ctx.lineWidth = 2.2 / camera.zoom;
        } else if (isDirectToSelected || isUpstream || isDownstream) {
          ctx.strokeStyle = '#ffffff';
          ctx.lineWidth = 2.0 / camera.zoom;
        } else if (isFaded) {
          ctx.strokeStyle = 'rgba(255, 255, 255, 0.08)';
          ctx.lineWidth = 1 / camera.zoom;
        } else {
          ctx.strokeStyle = 'rgba(255, 255, 255, 0.28)';
          ctx.lineWidth = 1.1 / camera.zoom;
        }

        ctx.stroke();

        // Arrow head
        drawArrowHead(cp2x, cp2y, x2, y2, ctx.strokeStyle);

        // Animated flow dashes when wire is hovered or selected
        if (isHoverConnected || isDirectToSelected || isUpstream || isDownstream) {
          ctx.save();
          ctx.setLineDash([5, 8]);
          ctx.lineDashOffset = -animTime * 24;
          ctx.strokeStyle = '#ffffff';
          ctx.lineWidth = 1.6 / camera.zoom;
          ctx.stroke();
          ctx.restore();
        }
      });
    });

    ctx.restore();
  }

  function drawArrowHead(fromX, fromY, toX, toY, color) {
    const angle = Math.atan2(toY - fromY, toX - fromX);
    const headLen = 6;
    ctx.save();
    ctx.fillStyle = color;
    ctx.beginPath();
    ctx.moveTo(toX, toY);
    ctx.lineTo(toX - headLen * Math.cos(angle - Math.PI / 6), toY - headLen * Math.sin(angle - Math.PI / 6));
    ctx.lineTo(toX - headLen * Math.cos(angle + Math.PI / 6), toY - headLen * Math.sin(angle + Math.PI / 6));
    ctx.closePath();
    ctx.fill();
    ctx.restore();
  }

  function drawNodes() {
    data.nodes.forEach(node => {
      const isSelected = selectedNode && selectedNode.id === node.id;
      const isHovered = hoveredNode && hoveredNode.id === node.id;
      const isUpstream = upstreamSet.has(node.id);
      const isDownstream = downstreamSet.has(node.id);
      const isHoverConnectedNode = hoveredNode && (
        hoveredNode.dependsOn.includes(node.id) || hoveredNode.unlocks.includes(node.id)
      );

      const isMatchingSearch = searchQuery && (
        node.title.toLowerCase().includes(searchQuery) ||
        node.id.toLowerCase().includes(searchQuery)
      );

      const isCategoryFiltered = activeFilterCategory !== 'all' && node.category !== activeFilterCategory;
      const isFaded = (selectedNode && !isSelected && !isUpstream && !isDownstream && !isHovered && !isHoverConnectedNode) || isCategoryFiltered;

      const cat = categoryMap.get(node.category) || { color: '#ffffff', name: 'CORE' };

      ctx.save();
      ctx.globalAlpha = isFaded ? 0.2 : 1.0;

      const x = Math.round(node.position.x);
      const y = Math.round(node.position.y);
      const w = NODE_WIDTH;
      const h = NODE_HEIGHT;

      // Card Background (Pure Black)
      ctx.fillStyle = '#000000';
      ctx.fillRect(x, y, w, h);

      // Card 1px Sharp Border
      if (isSelected || isHovered) {
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 2;
      } else if (isUpstream || isDownstream || isHoverConnectedNode) {
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 1.5;
      } else if (isMatchingSearch) {
        ctx.strokeStyle = '#ffffff';
        ctx.lineWidth = 1.5;
      } else {
        ctx.strokeStyle = 'rgba(255, 255, 255, 0.35)';
        ctx.lineWidth = 1;
      }
      ctx.strokeRect(x + 0.5, y + 0.5, w - 1, h - 1);

      // Top Category Accent Color Bar
      ctx.fillStyle = cat.color;
      ctx.fillRect(x, y, w, 3);

      // Category Tag (Monospace, white text, 1px border)
      ctx.strokeStyle = 'rgba(255, 255, 255, 0.25)';
      ctx.lineWidth = 1;
      ctx.strokeRect(x + 8.5, y + 9.5, 74, 14);
      
      ctx.fillStyle = '#ffffff';
      ctx.font = '700 8px ui-monospace, "SF Mono", Consolas, monospace';
      ctx.fillText(cat.name.toUpperCase().substring(0, 13), x + 12, y + 19.5);

      // Status text indicator
      const statusLabels = {
        'completed': 'STABLE',
        'in-progress': 'ACTIVE',
        'planned': 'PLANNED',
        'research': 'RESEARCH'
      };
      ctx.fillStyle = 'rgba(255, 255, 255, 0.65)';
      ctx.font = '700 8px ui-monospace, "SF Mono", Consolas, monospace';
      const sText = statusLabels[node.status] || node.status.toUpperCase();
      ctx.fillText(sText, x + w - ctx.measureText(sText).width - 9, y + 19.5);

      // Title (Pure White, Bold, crisp rendering)
      ctx.fillStyle = '#ffffff';
      ctx.font = '700 12px ui-monospace, "SF Mono", Consolas, monospace';
      const title = truncateText(ctx, node.title.toUpperCase(), w - 18);
      ctx.fillText(title, x + 9, y + 42);

      // Summary (Muted White)
      ctx.fillStyle = 'rgba(255, 255, 255, 0.65)';
      ctx.font = '400 9.5px -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif';
      const summaryLine = truncateText(ctx, node.summary || '', w - 18);
      ctx.fillText(summaryLine, x + 9, y + 59);

      // Subtask mini-progress bar (1px sharp border)
      const subtasks = node.details?.subtasks || [];
      const completedCount = subtasks.filter(t => t.done).length;
      const totalCount = subtasks.length;
      const pct = totalCount > 0 ? (completedCount / totalCount) : (node.status === 'completed' ? 1.0 : 0.0);

      const barX = x + 9;
      const barY = y + h - 13;
      const barW = w - 18;
      const barH = 3;

      ctx.strokeStyle = 'rgba(255, 255, 255, 0.25)';
      ctx.lineWidth = 1;
      ctx.strokeRect(barX + 0.5, barY + 0.5, barW - 1, barH - 1);

      if (pct > 0) {
        ctx.fillStyle = '#ffffff';
        ctx.fillRect(barX + 1, barY + 1, (barW - 2) * pct, barH - 2);
      }

      ctx.restore();
    });
  }

  function truncateText(context, text, maxWidth) {
    if (context.measureText(text).width <= maxWidth) return text;
    let truncated = text;
    while (truncated.length > 0 && context.measureText(truncated + '…').width > maxWidth) {
      truncated = truncated.substring(0, truncated.length - 1);
    }
    return truncated + '…';
  }

  // -------------------------------------------------------------
  // MiniMap Rendering (Minimalist Monochrome)
  // -------------------------------------------------------------
  function drawMinimap() {
    if (!data || data.nodes.length === 0) return;

    const w = 170;
    const h = 110;
    minimapCtx.fillStyle = '#000000';
    minimapCtx.fillRect(0, 0, w, h);

    let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
    data.nodes.forEach(n => {
      minX = Math.min(minX, n.position.x);
      minY = Math.min(minY, n.position.y);
      maxX = Math.max(maxX, n.position.x + NODE_WIDTH);
      maxY = Math.max(maxY, n.position.y + NODE_HEIGHT);
    });

    const padding = 150;
    minX -= padding;
    minY -= padding;
    maxX += padding;
    maxY += padding;

    const scaleX = w / (maxX - minX);
    const scaleY = h / (maxY - minY);
    const scale = Math.min(scaleX, scaleY);

    const offX = (w - (maxX - minX) * scale) / 2;
    const offY = (h - (maxY - minY) * scale) / 2;

    data.nodes.forEach(n => {
      const nx = offX + (n.position.x - minX) * scale;
      const ny = offY + (n.position.y - minY) * scale;
      const nw = Math.max(NODE_WIDTH * scale, 3);
      const nh = Math.max(NODE_HEIGHT * scale, 2);

      const cat = categoryMap.get(n.category) || { color: '#ffffff' };
      minimapCtx.fillStyle = cat.color;
      minimapCtx.fillRect(nx, ny, nw, nh);
    });

    const vpRect = viewport.getBoundingClientRect();
    const camLeft = -camera.x / camera.zoom;
    const camTop = -camera.y / camera.zoom;
    const camW = vpRect.width / camera.zoom;
    const camH = vpRect.height / camera.zoom;

    const vx = offX + (camLeft - minX) * scale;
    const vy = offY + (camTop - minY) * scale;
    const vw = camW * scale;
    const vh = camH * scale;

    minimapCtx.strokeStyle = '#ffffff';
    minimapCtx.lineWidth = 1;
    minimapCtx.strokeRect(vx, vy, vw, vh);
  }

  // -------------------------------------------------------------
  // Mouse & Touch Event Handlers
  // -------------------------------------------------------------
  function setupEvents() {
    viewport.addEventListener('mousedown', onMouseDown);
    window.addEventListener('mousemove', onMouseMove);
    window.addEventListener('mouseup', onMouseUp);
    viewport.addEventListener('wheel', onWheel, { passive: false });

    minimapCanvas.addEventListener('click', (e) => {
      const rect = minimapCanvas.getBoundingClientRect();
      const clickX = e.clientX - rect.left;
      const clickY = e.clientY - rect.top;

      let minX = Infinity, minY = Infinity, maxX = -Infinity, maxY = -Infinity;
      data.nodes.forEach(n => {
        minX = Math.min(minX, n.position.x);
        minY = Math.min(minY, n.position.y);
        maxX = Math.max(maxX, n.position.x + NODE_WIDTH);
        maxY = Math.max(maxY, n.position.y + NODE_HEIGHT);
      });
      const padding = 150;
      minX -= padding; minY -= padding; maxX += padding; maxY += padding;

      const scale = Math.min(170 / (maxX - minX), 110 / (maxY - minY));
      const offX = (170 - (maxX - minX) * scale) / 2;
      const offY = (110 - (maxY - minY) * scale) / 2;

      const worldX = minX + (clickX - offX) / scale;
      const worldY = minY + (clickY - offY) / scale;

      const vpRect = viewport.getBoundingClientRect();
      smoothPanTo(vpRect.width / 2 - worldX * camera.zoom, vpRect.height / 2 - worldY * camera.zoom);
    });
  }

  function screenToWorld(sx, sy) {
    const rect = viewport.getBoundingClientRect();
    const vx = sx - rect.left;
    const vy = sy - rect.top;
    return {
      x: (vx - camera.x) / camera.zoom,
      y: (vy - camera.y) / camera.zoom
    };
  }

  function getNodeAtPosition(wx, wy) {
    for (let i = data.nodes.length - 1; i >= 0; i--) {
      const n = data.nodes[i];
      if (
        wx >= n.position.x &&
        wx <= n.position.x + NODE_WIDTH &&
        wy >= n.position.y &&
        wy <= n.position.y + NODE_HEIGHT
      ) {
        return n;
      }
    }
    return null;
  }

  function onMouseDown(e) {
    if (e.target !== canvas && e.target !== viewport) return;

    const wPos = screenToWorld(e.clientX, e.clientY);
    const hit = getNodeAtPosition(wPos.x, wPos.y);

    if (hit) {
      if (e.button === 0) {
        draggedNode = hit;
        dragOffsetX = wPos.x - hit.position.x;
        dragOffsetY = wPos.y - hit.position.y;
        isDraggingNode = false;
      }
    } else {
      if (e.button === 0 || e.button === 1) {
        isPanning = true;
        startPanX = e.clientX - camera.x;
        startPanY = e.clientY - camera.y;
        viewport.classList.add('panning');
      }
    }
  }

  function onMouseMove(e) {
    const wPos = screenToWorld(e.clientX, e.clientY);

    if (draggedNode) {
      isDraggingNode = true;
      draggedNode.position.x = wPos.x - dragOffsetX;
      draggedNode.position.y = wPos.y - dragOffsetY;
      requestRedraw();
      return;
    }

    if (isPanning) {
      camera.x = e.clientX - startPanX;
      camera.y = e.clientY - startPanY;
      requestRedraw();
      return;
    }

    const hit = getNodeAtPosition(wPos.x, wPos.y);
    if (hit !== hoveredNode) {
      hoveredNode = hit;
      viewport.style.cursor = hit ? 'pointer' : 'grab';
      requestRedraw();
    }
  }

  function onMouseUp(e) {
    if (draggedNode) {
      if (!isDraggingNode) {
        selectNode(draggedNode);
      }
      draggedNode = null;
      isDraggingNode = false;
      return;
    }

    if (isPanning) {
      isPanning = false;
      viewport.classList.remove('panning');
    }
  }

  function onWheel(e) {
    e.preventDefault();
    const rect = viewport.getBoundingClientRect();
    const mouseX = e.clientX - rect.left;
    const mouseY = e.clientY - rect.top;

    const zoomFactor = e.deltaY < 0 ? 1.1 : 0.9;
    const newZoom = Math.min(Math.max(camera.zoom * zoomFactor, camera.minZoom), camera.maxZoom);

    camera.x = mouseX - (mouseX - camera.x) * (newZoom / camera.zoom);
    camera.y = mouseY - (mouseY - camera.y) * (newZoom / camera.zoom);
    camera.zoom = newZoom;

    requestRedraw();
  }

  window.addEventListener('DOMContentLoaded', init);
})();
