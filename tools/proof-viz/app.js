"use strict";

const scenarioRoot = document.querySelector("#scenarios");

(async () => {
    const response = await fetch("data.json");
    if (!response.ok) {
        throw new Error(`Could not load proof data (${response.status})`);
    }

    const data = await response.json();
    if (data.schemaVersion !== 5 || !Array.isArray(data.scenarios)) {
        throw new Error("Proof visualization data is missing");
    }

    // Nodes arrive as parallel columns with one bitmask each. The tile and
    // frontier bits are what those two stores answered for that range, so a
    // node carrying both really is served by either; the proof bit marks the
    // ranges the returned proof actually used.
    //
    // A consistency scene draws both trees. Ranges the smaller tree folds its
    // proof elements into are absent from the larger tree unless its size is
    // an aligned power of two, so they are drawn as individual nodes with no
    // edges: they belong to a structure this map does not otherwise show.
    const { tile, frontier, proof, first, second, inFirstTree, inSecondTree } = data.flags;
    data.scenarios.forEach((scenario) => {
        scenario.nodes = scenario.lo.map((lo, index) => {
            const bits = scenario.flags[index];
            return {
                lo,
                hi: scenario.hi[index],
                height: scenario.height[index],
                parent: scenario.parent[index],
                // Only where the two trees combine this range with a
                // different sibling: elsewhere the edge is already drawn.
                parentFirst: scenario.parentFirst[index] !== scenario.parent[index]
                    ? scenario.parentFirst[index]
                    : -1,
                source: (bits & frontier) ? "frontier" : (bits & tile) ? "tile" : "computed",
                overlap: (bits & (tile | frontier)) === (tile | frontier),
                proof: (bits & proof) !== 0,
                inFirst: (bits & inFirstTree) !== 0,
                inSecond: (bits & inSecondTree) !== 0,
                isRoot: index === scenario.firstRoot || index === scenario.secondRoot,
                rootLabel: scenario.type !== "consistency"
                    ? (index === scenario.secondRoot ? "root" : "")
                    : index === scenario.firstRoot
                        ? "old root"
                        : index === scenario.secondRoot ? "new root" : "",
                endpoint: (bits & first)
                    ? (scenario.type === "consistency" ? "A" : "T")
                    : (bits & second) ? "B" : ""
            };
        });
    });

    const canvasColorNames = [
        "tile", "frontier", "computed", "proof", "endpoint",
        "edge", "boundary", "muted", "paper", "primary"
    ];
    const readCanvasColors = () => {
        const styles = getComputedStyle(document.documentElement);
        return Object.fromEntries(canvasColorNames.map(
            (name) => [name, styles.getPropertyValue(`--${name}`).trim()]
        ));
    };
    let colors = readCanvasColors();

    const state = { edges: true };

    const number = new Intl.NumberFormat("en-US");
    const tooltip = document.querySelector("#node-tooltip");
    const renderers = [];

    window.matchMedia("(prefers-color-scheme: dark)").addEventListener(
        "change",
        (event) => {
            if (!localStorage.getItem("theme")) {
                document.documentElement.dataset.theme = event.matches ? "dark" : "light";
            }
            colors = readCanvasColors();
            renderers.forEach((renderer) => renderer.draw());
        }
    );

    scenarioRoot.replaceChildren();
    scenarioRoot.removeAttribute("aria-busy");
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

    function tooltipText(node, scenario) {
        const span = node.hi - node.lo;
        const roles = [];
        if (node.proof) roles.push("returned proof element");
        if (node.endpoint === "T") roles.push("target entry (T)");
        if (node.endpoint === "A") roles.push("earlier selected leaf (A)");
        if (node.endpoint === "B") roles.push("later selected leaf (B)");
        if (node.overlap) roles.push("available from both; memory selected first");
        if (roles.length === 0) roles.push("not returned in this proof");

        const source = node.source === "tile"
            ? "tile-backed"
            : node.source === "frontier"
                ? "resident frontier"
                : "computed from child ranges";

        const trees = [];
        if (scenario.type === "consistency") {
            if (node.inFirst && !node.inSecond) {
                trees.push("only in the tree ending at A: rebuilt by folding proof elements");
            } else if (node.inFirst && node.inSecond) {
                trees.push("shared by both trees");
            }
            if (node.isRoot) {
                trees.push(node.hi === scenario.focus + 1 ? "root of A" : "root of B");
            }
        }
        return {
            title: `[${number.format(node.lo)}, ${number.format(node.hi)})`,
            detail: `${number.format(span)} ${span === 1 ? "leaf" : "leaves"} · ${source} · ${roles.concat(trees).join(" · ")}`
        };
    }

    function showTooltip(event, node, scenario) {
        const text = tooltipText(node, scenario);
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

    function createRenderer(canvas, scroll, scenario) {
        let hitTargets = [];

        function draw() {
            const maxHeight = Math.max(...scenario.nodes.map((node) => node.height));
            const minimumWidth = Math.max(660, Math.ceil(scenario.mapLeaves * 2.35 + 88));
            const availableWidth = scroll.clientWidth - 2;
            const cssWidth = window.innerWidth > 980
                ? availableWidth
                : Math.max(availableWidth, minimumWidth);
            const cssHeight = Math.max(270, (maxHeight + 1) * 22 + 64);
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
            // Rank by height above the leaves, not depth below the root, so
            // every leaf shares the bottom row: the decomposition is
            // unbalanced, and a ragged range reaches its leaves in fewer
            // splits than a perfect one.
            const positions = scenario.nodes.map((node) => ({
                x: plot.left + (((node.lo + node.hi) / 2) / scenario.mapLeaves) * plotWidth,
                y: plot.top + ((maxHeight - node.height) / Math.max(maxHeight, 1)) * plotHeight
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
                context.fillStyle = colors.muted;
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

            // How the old root is reached. Wherever the two trees combine a
            // range with a different sibling, the old tree's own edge is drawn
            // on top, so the folds that rebuild the old root can be traced
            // from the proof elements that feed them.
            context.save();
            context.lineWidth = 1.4;
            context.strokeStyle = colors.primary;
            context.setLineDash([3, 3]);
            context.beginPath();
            scenario.nodes.forEach((node, index) => {
                if (node.parentFirst < 0) return;
                const parent = positions[node.parentFirst];
                const current = positions[index];
                context.moveTo(parent.x, parent.y);
                context.lineTo(current.x, current.y);
            });
            context.stroke();
            context.restore();

            const baseSize = scenario.mapLeaves > 400 ? 3 : 3.6;
            hitTargets = [];
            scenario.nodes.forEach((node, index) => {
                const position = positions[index];
                const roleSize = baseSize + 4;

                if (node.proof) {
                    context.fillStyle = colors.proof;
                    context.fillRect(
                        position.x - roleSize / 2,
                        position.y - roleSize / 2,
                        roleSize,
                        roleSize
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

                // A range no store answers and no proof names is only where two
                // halves are combined. It has to stay for the tree to keep its
                // shape, but it is drawn small so it reads as a join rather
                // than as data someone holds.
                const isJoin = node.source === "computed" &&
                    !node.proof && !node.endpoint && !node.isRoot;
                const size = isJoin ? baseSize * 0.55 : baseSize;
                context.fillStyle = colors[node.source];
                context.fillRect(
                    position.x - size / 2,
                    position.y - size / 2,
                    size,
                    size
                );

                // Nodes that exist only in the tree ending at A: the ranges a
                // verifier folds the proof back into. Ringed rather than
                // filled, since they carry no edges in this map.
                if (node.inFirst && !node.inSecond) {
                    const ringSize = baseSize + 5;
                    context.save();
                    context.strokeStyle = colors.endpoint;
                    context.lineWidth = 1.2;
                    context.strokeRect(
                        position.x - ringSize / 2,
                        position.y - ringSize / 2,
                        ringSize,
                        ringSize
                    );
                    context.restore();
                }

                // The roots carry the whole claim -- a consistency proof exists
                // to turn one into the other -- so name them on the canvas.
                if (node.rootLabel) {
                    const markerSize = baseSize + 9;
                    const toLeft = position.x > cssWidth - 120;
                    context.save();
                    context.strokeStyle = colors.primary;
                    context.fillStyle = colors.primary;
                    context.lineWidth = 2;
                    context.beginPath();
                    context.arc(position.x, position.y, markerSize / 2, 0, Math.PI * 2);
                    context.stroke();
                    context.font = "700 10px 'Cascadia Code', monospace";
                    context.textAlign = toLeft ? "right" : "left";
                    context.fillText(
                        node.rootLabel,
                        position.x + (toLeft ? -1 : 1) * (markerSize / 2 + 5),
                        position.y + 3
                    );
                    context.restore();
                }

                if (node.endpoint) {
                    const markerSize = baseSize + 8;
                    context.save();
                    context.strokeStyle = colors.endpoint;
                    context.fillStyle = colors.endpoint;
                    context.lineWidth = 2;
                    context.beginPath();
                    if (node.endpoint !== "B") {
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

            context.fillStyle = colors.muted;
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
                showTooltip(event, nearest.node, scenario);
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
                fact("Tiled prefix", `${number.format(scenario.covered)} · ${number.format(scenario.tiles.length)} tiles`),
                fact("Frontier begins", number.format(scenario.frontierStart)),
                fact("Target leaf", number.format(scenario.focus))
            );
        } else {
            facts.append(
                fact("Backing leaves", number.format(scenario.leaves)),
                fact("Tiled prefix", `${number.format(scenario.covered)} · ${number.format(scenario.tiles.length)} tiles`),
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
        mapMeta.textContent = `${number.format(scenario.nodes.length)} nodes · ${number.format(scenario.proof.length)} proof elements`;
        visualHead.append(mapTitle, mapMeta);

        const scroll = document.createElement("div");
        scroll.className = "canvas-scroll";
        const canvas = document.createElement("canvas");
        canvas.className = "tree-canvas";
        canvas.setAttribute("role", "img");
        canvas.setAttribute(
            "aria-label",
            `${scenario.title}: ${number.format(scenario.mapLeaves)} proof leaves with returned proof elements highlighted`
        );
        scroll.append(canvas);
        visual.append(visualHead, scroll);

        inner.append(copy, visual);
        article.append(inner);
        scenarioRoot.append(article);

        const renderer = createRenderer(canvas, scroll, scenario);
        renderers.push({ ...renderer, article });
    }

    data.scenarios.forEach(makeScene);

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

})().catch((error) => {
    console.error(error);
    const message = document.createElement("p");
    message.className = "load-status load-status--error";
    message.setAttribute("role", "alert");
    message.textContent = `Could not load proof visualization: ${error.message}`;
    scenarioRoot.removeAttribute("aria-busy");
    scenarioRoot.replaceChildren(message);
});