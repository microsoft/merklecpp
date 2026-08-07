(() => {
    "use strict";

    const data = window.PROOF_VIZ_DATA;
    if (!data || !Array.isArray(data.scenarios)) {
        throw new Error("Proof visualization data is missing");
    }

    const colors = {
        tile: "#238b57",
        frontier: "#2868a8",
        computed: "#aaa79c",
        route: "#d13b3f",
        query: "#e4a11b",
        endpoint: "#202521",
        edge: "rgba(74, 79, 73, 0.20)",
        boundary: "rgba(32, 37, 33, 0.48)",
        paper: "#fffdf7"
    };

    const state = {
        edges: true,
        calls: true
    };

    const number = new Intl.NumberFormat("en-US");
    const scenarioRoot = document.querySelector("#scenarios");
    const tooltip = document.querySelector("#node-tooltip");
    const renderers = [];

    document.querySelector("#scene-count").textContent = data.scenarios.length;
    document.querySelector("#tile-width").textContent = number.format(data.tileWidth);

    function fact(term, value) {
        const wrapper = document.createElement("div");
        const name = document.createElement("dt");
        const content = document.createElement("dd");
        name.textContent = term;
        content.textContent = value;
        wrapper.append(name, content);
        return wrapper;
    }

    function sourceAnswer(attempt) {
        return `${attempt.source} ${attempt.success ? "answered" : "missed"} [${attempt.lo}, ${attempt.hi}) at level ${attempt.level}`;
    }

    function makeAttempts(scenario) {
        const section = document.createElement("div");
        section.className = "attempts";

        const head = document.createElement("div");
        head.className = "attempts__head";
        const title = document.createElement("strong");
        title.textContent = "Resolver attempt order";
        const note = document.createElement("span");
        note.textContent = "hollow = miss; solid = answer";
        head.append(title, note);

        const track = document.createElement("div");
        track.className = "attempts__track";
        scenario.attempts.forEach((attempt, index) => {
            const marker = document.createElement("span");
            marker.className = `attempt attempt--${attempt.source}${attempt.success ? " is-success" : ""}`;
            marker.title = `${index + 1}. ${sourceAnswer(attempt)}`;
            marker.setAttribute("aria-label", marker.title);
            track.append(marker);
        });

        section.append(head, track);
        return section;
    }

    function tooltipText(node) {
        const span = node.hi - node.lo;
        const roles = [];
        if (node.path) roles.push("proof route");
        if (node.proof) roles.push("proof component");
        if (node.endpoint) roles.push(`${node.endpoint === "A" ? "earlier" : "later"} selected leaf (${node.endpoint})`);
        if (node.queried) roles.push(`resolved #${node.queryOrder} by ${node.querySource}`);
        if (node.overlap) roles.push("available from both; memory selected first");
        if (roles.length === 0) roles.push("not used by this proof");

        const source = node.source === "tile"
            ? "tile-backed"
            : node.source === "frontier"
                ? "resident frontier"
                : "computed from child ranges";
        return {
            title: `[${number.format(node.lo)}, ${number.format(node.hi)})`,
            detail: `${number.format(span)} ${span === 1 ? "leaf" : "leaves"} · ${source} · ${roles.join(" · ")}`
        };
    }

    function showTooltip(event, node) {
        const text = tooltipText(node);
        tooltip.replaceChildren();
        const title = document.createElement("b");
        title.textContent = text.title;
        const detail = document.createElement("span");
        detail.textContent = text.detail;
        tooltip.append(title, detail);
        tooltip.hidden = false;

        const margin = 14;
        const width = tooltip.offsetWidth;
        const height = tooltip.offsetHeight;
        tooltip.style.left = `${Math.min(event.clientX + 14, window.innerWidth - width - margin)}px`;
        tooltip.style.top = `${Math.min(event.clientY + 14, window.innerHeight - height - margin)}px`;
    }

    function hideTooltip() {
        tooltip.hidden = true;
    }

    function splitAt(lo, hi) {
        return lo + 2 ** Math.floor(Math.log2(hi - lo - 1));
    }

    function buildNodes(scenario) {
        scenario.covered = Math.floor(scenario.leaves / data.tileWidth) * data.tileWidth;
        scenario.frontierStart = Math.min(scenario.covered, scenario.leaves - 1);
        scenario.type = scenario.secondIndex === undefined ? "inclusion" : "consistency";
        scenario.mapLeaves = (scenario.secondIndex ?? scenario.leaves - 1) + 1;
        const nodes = [];
        const byRange = new Map();
        const key = (lo, hi) => `${lo}:${hi}`;

        scenario.attempts.forEach((attempt) => {
            const width = 2 ** attempt.level;
            attempt.lo = attempt.index * width;
            attempt.hi = attempt.lo + width;
        });

        function add(lo, hi, depth, parent = -1) {
            const width = hi - lo;
            const complete = 2 ** Math.floor(Math.log2(width)) === width && lo % width === 0;
            const inFrontier = complete && lo >= scenario.frontierStart;
            const inTiles = complete && hi <= scenario.covered;
            const node = {
                lo,
                hi,
                depth,
                parent,
                source: inFrontier ? "frontier" : inTiles ? "tile" : "computed",
                overlap: inFrontier && inTiles,
                path: false,
                proof: false,
                endpoint: "",
                queried: false,
                queryOrder: 0,
                querySource: ""
            };
            const index = nodes.push(node) - 1;
            byRange.set(key(lo, hi), node);
            if (width > 1) {
                const split = splitAt(lo, hi);
                add(lo, split, depth + 1, index);
                add(split, hi, depth + 1, index);
            }
        }

        const mark = (lo, hi, role) => {
            byRange.get(key(lo, hi))[role] = true;
        };

        function markInclusion(lo, hi) {
            mark(lo, hi, "path");
            if (hi - lo === 1) return;
            const split = splitAt(lo, hi);
            if (scenario.focus < split) {
                mark(split, hi, "proof");
                markInclusion(lo, split);
            } else {
                mark(lo, split, "proof");
                markInclusion(split, hi);
            }
        }

        function markConsistency(firstSize, lo, hi, complete) {
            mark(lo, hi, "path");
            if (firstSize === hi - lo) {
                if (!complete) mark(lo, hi, "proof");
                return;
            }
            const split = splitAt(lo, hi);
            const leftSize = split - lo;
            if (firstSize <= leftSize) {
                markConsistency(firstSize, lo, split, complete);
                mark(split, hi, "proof");
            } else {
                markConsistency(firstSize - leftSize, split, hi, false);
                mark(lo, split, "proof");
            }
        }

        add(0, scenario.mapLeaves, 0);
        if (scenario.type === "inclusion") {
            markInclusion(0, scenario.mapLeaves);
        } else {
            markConsistency(scenario.focus + 1, 0, scenario.mapLeaves, true);
            byRange.get(key(scenario.focus, scenario.focus + 1)).endpoint = "A";
            byRange.get(key(scenario.secondIndex, scenario.secondIndex + 1)).endpoint = "B";
        }

        scenario.attempts.forEach((attempt, order) => {
            const node = attempt.success && byRange.get(key(attempt.lo, attempt.hi));
            if (!node) return;
            if (!node.queried) {
                node.queryOrder = order + 1;
                node.querySource = attempt.source;
            }
            node.queried = true;
        });
        return nodes;
    }

    function createRenderer(canvas, scroll, scenario) {
        let hitTargets = [];

        function draw() {
            const maxDepth = Math.max(...scenario.nodes.map((node) => node.depth));
            const minimumWidth = Math.max(660, Math.ceil(scenario.mapLeaves * 2.35 + 88));
            const availableWidth = scroll.clientWidth - 2;
            const cssWidth = window.innerWidth > 980
                ? availableWidth
                : Math.max(availableWidth, minimumWidth);
            const cssHeight = Math.max(270, (maxDepth + 1) * 22 + 64);
            const pixelRatio = Math.min(window.devicePixelRatio || 1, 2);

            canvas.width = Math.round(cssWidth * pixelRatio);
            canvas.height = Math.round(cssHeight * pixelRatio);
            canvas.style.width = `${cssWidth}px`;
            canvas.style.height = `${cssHeight}px`;
            const context = canvas.getContext("2d");
            context.setTransform(pixelRatio, 0, 0, pixelRatio, 0, 0);
            context.clearRect(0, 0, cssWidth, cssHeight);

            const plot = { left: 42, right: 22, top: 25, bottom: 38 };
            const plotWidth = cssWidth - plot.left - plot.right;
            const plotHeight = cssHeight - plot.top - plot.bottom;
            const positions = scenario.nodes.map((node) => ({
                x: plot.left + (((node.lo + node.hi) / 2) / scenario.mapLeaves) * plotWidth,
                y: plot.top + (node.depth / Math.max(maxDepth, 1)) * plotHeight
            }));

            if (scenario.covered > 0 && scenario.covered < scenario.mapLeaves) {
                const boundaryX = plot.left + (scenario.covered / scenario.mapLeaves) * plotWidth;
                context.save();
                context.setLineDash([4, 4]);
                context.strokeStyle = colors.boundary;
                context.beginPath();
                context.moveTo(boundaryX, 8);
                context.lineTo(boundaryX, cssHeight - 20);
                context.stroke();
                context.restore();
                context.fillStyle = "#50564f";
                context.font = "10px 'Cascadia Code', monospace";
                context.textAlign = boundaryX > cssWidth - 140 ? "right" : "left";
                context.fillText(
                    `flush line ${number.format(scenario.covered)}`,
                    boundaryX + (boundaryX > cssWidth - 140 ? -6 : 6),
                    14
                );
            }

            if (state.edges) {
                context.lineWidth = 0.7;
                context.strokeStyle = colors.edge;
                context.beginPath();
                scenario.nodes.forEach((node, index) => {
                    if (node.parent < 0) return;
                    const parent = positions[node.parent];
                    const current = positions[index];
                    context.moveTo(parent.x, parent.y);
                    context.lineTo(current.x, current.y);
                });
                context.stroke();
            }

            context.lineWidth = 1.7;
            context.strokeStyle = colors.route;
            context.beginPath();
            scenario.nodes.forEach((node, index) => {
                if (!node.path || node.parent < 0 || !scenario.nodes[node.parent].path) return;
                const parent = positions[node.parent];
                const current = positions[index];
                context.moveTo(parent.x, parent.y);
                context.lineTo(current.x, current.y);
            });
            context.stroke();

            const baseSize = scenario.mapLeaves > 400 ? 3 : 3.6;
            hitTargets = [];
            scenario.nodes.forEach((node, index) => {
                const position = positions[index];
                const roleSize = baseSize + 4;

                if (node.proof || node.path) {
                    context.fillStyle = colors.route;
                    context.fillRect(
                        position.x - roleSize / 2,
                        position.y - roleSize / 2,
                        roleSize,
                        roleSize
                    );
                }

                if (state.calls && node.queried) {
                    const querySize = roleSize + 4;
                    context.strokeStyle = colors.query;
                    context.lineWidth = 2;
                    context.strokeRect(
                        position.x - querySize / 2,
                        position.y - querySize / 2,
                        querySize,
                        querySize
                    );
                }

                if (node.overlap) {
                    const overlapSize = baseSize + 2.5;
                    context.fillStyle = colors.tile;
                    context.fillRect(
                        position.x - overlapSize / 2,
                        position.y - overlapSize / 2,
                        overlapSize,
                        overlapSize
                    );
                }

                context.fillStyle = colors[node.source];
                context.fillRect(
                    position.x - baseSize / 2,
                    position.y - baseSize / 2,
                    baseSize,
                    baseSize
                );

                if (node.endpoint) {
                    const markerSize = baseSize + 8;
                    context.save();
                    context.strokeStyle = colors.endpoint;
                    context.fillStyle = colors.endpoint;
                    context.lineWidth = 2;
                    context.beginPath();
                    if (node.endpoint === "A") {
                        context.arc(position.x, position.y, markerSize / 2, 0, Math.PI * 2);
                    } else {
                        context.moveTo(position.x, position.y - markerSize / 2);
                        context.lineTo(position.x + markerSize / 2, position.y);
                        context.lineTo(position.x, position.y + markerSize / 2);
                        context.lineTo(position.x - markerSize / 2, position.y);
                        context.closePath();
                    }
                    context.stroke();
                    context.font = "700 10px 'Cascadia Code', monospace";
                    context.textAlign = "center";
                    context.fillText(
                        `${node.endpoint} ${number.format(node.lo)}`,
                        position.x,
                        position.y - markerSize / 2 - 5
                    );
                    context.restore();
                }
                hitTargets.push({ ...position, node });
            });

            context.fillStyle = "#50564f";
            context.font = "10px 'Cascadia Code', monospace";
            context.textAlign = "left";
            context.fillText("leaf 0", plot.left, cssHeight - 12);
            context.textAlign = "right";
            context.fillText(`leaf ${number.format(scenario.mapLeaves - 1)}`, cssWidth - plot.right, cssHeight - 12);
        }

        canvas.addEventListener("pointermove", (event) => {
            const bounds = canvas.getBoundingClientRect();
            const x = event.clientX - bounds.left;
            const y = event.clientY - bounds.top;
            let nearest = null;
            let nearestDistance = 9 * 9;
            for (let index = hitTargets.length - 1; index >= 0; index--) {
                const target = hitTargets[index];
                const distance = (target.x - x) ** 2 + (target.y - y) ** 2;
                if (distance < nearestDistance) {
                    nearest = target;
                    nearestDistance = distance;
                }
            }
            if (nearest) {
                showTooltip(event, nearest.node);
            } else {
                hideTooltip();
            }
        });
        canvas.addEventListener("pointerleave", hideTooltip);

        const observer = new ResizeObserver(draw);
        observer.observe(scroll);
        draw();
        return { draw, observer };
    }

    function makeScene(scenario, index) {
        const article = document.createElement("article");
        article.className = "scene";
        article.dataset.type = scenario.type;
        article.id = scenario.id;

        const inner = document.createElement("div");
        inner.className = "scene__inner";

        const copy = document.createElement("div");
        copy.className = "scene__copy";
        const intro = document.createElement("div");
        const type = document.createElement("p");
        type.className = "scene__type";
        type.innerHTML = `<span class="scene__index">${String(index + 1).padStart(2, "0")}</span> / ${scenario.type} proof`;
        const title = document.createElement("h2");
        title.textContent = scenario.title;
        const description = document.createElement("p");
        description.className = "scene__description";
        description.textContent = scenario.description;
        intro.append(type, title, description);

        const details = document.createElement("div");
        const facts = document.createElement("dl");
        facts.className = "scene__facts";
        if (scenario.type === "inclusion") {
            facts.append(
                fact("Leaves", number.format(scenario.leaves)),
                fact("Tiled prefix", number.format(scenario.covered)),
                fact("Frontier begins", number.format(scenario.frontierStart)),
                fact("Target leaf", number.format(scenario.focus))
            );
        } else {
            facts.append(
                fact("Backing leaves", number.format(scenario.leaves)),
                fact("Tiled prefix", number.format(scenario.covered)),
                fact("Earlier leaf A", `index ${number.format(scenario.focus)} · tree ends at A`),
                fact("Later leaf B", `index ${number.format(scenario.secondIndex)} · tree ends at B`)
            );
        }
        const takeaway = document.createElement("p");
        takeaway.className = "scene__takeaway";
        takeaway.textContent = scenario.takeaway;
        details.append(facts, takeaway);
        copy.append(intro, details);

        const visual = document.createElement("div");
        visual.className = "scene__visual";
        const visualHead = document.createElement("div");
        visualHead.className = "visual-head";
        const mapTitle = document.createElement("span");
        mapTitle.innerHTML = scenario.type === "consistency"
            ? "<strong>Leaf-to-leaf consistency</strong> / A and B anchor the two tree states"
            : "<strong>Node map</strong> / root to leaves";
        const mapMeta = document.createElement("span");
        mapMeta.textContent = `${number.format(scenario.nodes.length)} nodes · ${number.format(scenario.attempts.length)} resolver attempts`;
        visualHead.append(mapTitle, mapMeta);

        const scroll = document.createElement("div");
        scroll.className = "canvas-scroll";
        const canvas = document.createElement("canvas");
        canvas.className = "tree-canvas";
        canvas.setAttribute("role", "img");
        canvas.setAttribute(
            "aria-label",
            `${scenario.title}: ${number.format(scenario.mapLeaves)} proof leaves with tile, frontier, and proof-route nodes`
        );
        scroll.append(canvas);
        const attempts = makeAttempts(scenario);
        visual.append(visualHead, scroll, attempts);

        inner.append(copy, visual);
        article.append(inner);
        scenarioRoot.append(article);

        const renderer = createRenderer(canvas, scroll, scenario);
        renderers.push({ ...renderer, article, attempts });
    }

    data.scenarios.forEach((scenario, index) => {
        scenario.nodes = buildNodes(scenario);
        makeScene(scenario, index);
    });

    const filterButtons = document.querySelectorAll("[data-filter]");
    filterButtons.forEach((button) => {
        button.addEventListener("click", () => {
            const filter = button.dataset.filter;
            filterButtons.forEach((candidate) => {
                const active = candidate === button;
                candidate.classList.toggle("is-active", active);
                candidate.setAttribute("aria-selected", String(active));
            });
            renderers.forEach((renderer) => {
                renderer.article.hidden = filter !== "all" && renderer.article.dataset.type !== filter;
            });
        });
    });

    document.querySelector("#toggle-edges").addEventListener("change", (event) => {
        state.edges = event.target.checked;
        renderers.forEach((renderer) => renderer.draw());
    });

    document.querySelector("#toggle-calls").addEventListener("change", (event) => {
        state.calls = event.target.checked;
        renderers.forEach((renderer) => {
            renderer.attempts.hidden = !state.calls;
            renderer.draw();
        });
    });
})();