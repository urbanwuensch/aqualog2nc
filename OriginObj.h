<!DOCTYPE html>
<html lang="en">
<head>
    <meta charset="UTF-8">
    <meta name="viewport" content="width=device-width, initial-scale=1.0">
    <title></title>

    <!-- Google Fonts -->
    <link rel="preconnect" href="https://fonts.googleapis.com">
    <link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
    <link href="https://fonts.googleapis.com/css2?family=Inter:wght@400;500;600;700&family=Lora:wght@400;600&display=swap" rel="stylesheet">

    <!-- Font Awesome for Icons -->
    <link rel="stylesheet" href="https://cdnjs.cloudflare.com/ajax/libs/font-awesome/6.5.1/css/all.min.css">

    <link rel="stylesheet" href="phoenix-pro.css">
</head>
<body>
    <!-- Progress Indicator -->
    <div class="progress-container phcode-no-lp-edit ">
        <div class="progress-indicator">
            <div class="progress-dot" data-section="2">
                <span class="dot-number">1</span>
            </div>
            <div class="progress-line"></div>
            <div class="progress-dot" data-section="3">
                <span class="dot-number">2</span>
            </div>
            <div class="progress-line"></div>
            <div class="progress-dot" data-section="4">
                <span class="dot-number">3</span>
            </div>
            <div class="progress-line"></div>
            <div class="progress-dot" data-section="5">
                <span class="dot-number">4</span>
            </div>
            <div class="progress-line"></div>
            <div class="progress-dot" data-section="6">
                <span class="dot-number">5</span>
            </div>
        </div>
    </div>

    <!-- Main Container -->
    <main class="game-container phcode-no-lp-edit-this">
        <!-- Section 1: What is Live Preview Editing -->
        <div class="paper-card section active phcode-no-lp-edit" id="section-1" data-section="1" style="display: none;">
            <div class="section-header ">
                <h2 class="section-title">Edit in Live Preview</h2>
                <p class="section-subtitle">Edit your page visually -&nbsp; <b>Your HTML updates instantly</b></p>
            </div>

            <div class="features-grid">
                <div class="feature-item">
                    <img src="images/editText.png" alt="Edit Text">
                </div>
                <div class="feature-item">
                    <img src="images/imageLib.svg" alt="Image Library">
                </div>
                <div class="feature-item">
                    <img src="images/drahNDrop.png" alt="Drag and Drop">
                </div>
                <div class="feature-item">
                    <img src="images/layoutRulers.png" alt="Layout Rulers">
                </div>
            </div>

            <div class="get-started-container phcode-no-lp-edit phcode-dismiss-lp-edit">
                <button class="get-started-button" id="get-started-button">
                    <span>Get Started</span>
                    <i class="fas fa-arrow-right"></i>
                </button>
            </div>
        </div>

        <!-- Section 2: Live Preview Edit -->
        <div class="paper-card section phcode-no-lp-edit-this" id="section-2" data-section="2">
            <div class="section-header phcode-no-lp-edit">
                <h2 class="section-title">Edit Text</h2>
                <p class="section-subtitle">Double-click to edit the text below and add your name</p>
            </div>

            <div class="playground phcode-no-lp-edit-this">
                <div class="editable-container">
                    <span id="editable-name" class="editable-text">Hello ___</span>
                </div>
            </div>

            <div class="success-message phcode-no-lp-edit" id="success-message">
                <span class="success-icon">←</span>
                <p id="success-message-text">Your edit updated the code — undo with <span id="undo-shortcut"></span>, redo with <span id="redo-shortcut"></span>.</p>
            </div>

            <div class="slide-controls phcode-no-lp-edit"></div>

            <div class="hint-content-inline phcode-no-lp-edit" data-section="2">
                <p style="margin-bottom: 12px; font-weight: 600; color: var(--text-primary);">To complete this task:</p>
                <ol>
                    <li>Double-click on the &quot;Hello ___&quot; text</li>
                    <li>Replace the underscores with your name</li>
                    <li>Press Enter or click outside to apply your changes</li>
                </ol>
            </div>
        </div>

        <!-- Section 3: Change Images Live -->
        <div class="paper-card section phcode-no-lp-edit-this" id="section-3" data-section="3">
            <div class="section-header phcode-no-lp-edit">
                <h2 class="section-title">Change images live</h2>
                <p class="section-subtitle">Click the image below to replace it with one you like from the Image Library.</p>
            </div>

            <div class="playground phcode-no-lp-edit-this">
                <div class="image-container phcode-no-lp-edit-this">
                    <img id="replaceable-image" src="images/placeholder.svg" alt="Replaceable image" class="replaceable-image">
                </div>
            </div>

            <div class="success-message phcode-no-lp-edit" id="success-message-section3">
                <p id="success-message-text-section3-url" style="display: none;">Did you know that you can download images with the <img src="images/download.svg" alt="Download" class="inline-icon"> Download button in the Image Library?</p>
                <p id="success-message-text-section3-local" style="display: none;">Did you know you can click on an image to use it without downloading?</p>
            </div>

            <div class="slide-controls phcode-no-lp-edit"></div>

            <div class="hint-content-inline phcode-no-lp-edit" data-section="3">
                <p style="margin-bottom: 12px; font-weight: 600; color: var(--text-primary);">To complete this task:</p>
                <ul>
                    <li>Click the image to open Image Library (toggle with <img src="images/image.svg" alt="Image" class="inline-icon"> toolbar icon or double-click).</li>
                    <li>In the Image Library: hover over thumbnails to preview them</li>
                    <li>Click a thumbnail to place it on your page, or click the <img src="images/download.svg" alt="Download" class="inline-icon"> icon to download it first</li>
                </ul>
            </div>
        </div>

        <!-- Section 4: Edit Elements Visually -->
        <div class="paper-card section phcode-no-lp-edit-this" id="section-4" data-section="4">
            <div class="section-header phcode-no-lp-edit">
                <h2 class="section-title">Edit elements visually</h2>
                <p class="section-subtitle">Duplicate<img src="images/duplicate.svg" alt="Duplicate" class="inline-icon"> the card below to make <strong>3 cards</strong> total, then <strong>delete<img src="images/trash.svg" alt="Delete" class="inline-icon"> 1 card</strong>.</p>
            </div>

            <div class="progress-toast phcode-no-lp-edit" id="progress-toast-section4">
                <p id="progress-toast-text-section4">
                    <span id="card-count-status">1/3 cards</span>,
                    <span id="delete-count-status">0/1 deleted</span>
                </p>
            </div>

            <div class="undo-message phcode-no-lp-edit" id="undo-message-section4">
                <span class="warning-icon">⚠️</span>
                <p>Oops! All cards deleted. Press <strong><span id="undo-shortcut-section4"></span></strong> to undo.</p>
            </div>

            <div class="playground phcode-no-lp-edit-this">
                <div class="cards-container phcode-no-lp-edit-this">
                    <div class="demo-card">
                        <img src="images/placeholder.svg" alt="Card image" class="card-image">
                        <h3 class="card-title">Card</h3>
                        <p class="card-text">Click the <img src="images/arrowUp.svg" alt="Up" class="inline-icon">&nbsp;icon in toolbar above to select the parent card.</p>
                    </div>
                </div>
            </div>

            <div class="success-message phcode-no-lp-edit" id="success-message-section4">
                <span class="success-icon">✓</span>
                <p id="success-message-text-section4">Perfect! You&#39;ve mastered duplicating and deleting elements.</p>
            </div>

            <div class="slide-controls phcode-no-lp-edit"></div>

            <div class="hint-content-inline phcode-no-lp-edit" data-section="4">
                <p style="margin-bottom: 12px; font-weight: 600; color: var(--text-primary);">To complete this task:</p>
                <ol style="margin: 0 0 0 20px; padding: 0;">
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Click the card to select it</li>
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Use the <img src="images/duplicate.svg" alt="Duplicate" class="inline-icon"> button or press <strong><span id="duplicate-shortcut"></span></strong> to duplicate cards until you have 3 total</li>
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Select any card and use the <img src="images/trash.svg" alt="Delete" class="inline-icon"> button or press <strong>Delete</strong> to remove it</li>
                    <li style="margin: 0; color: var(--text-secondary); font-size: 0.9rem;">Tip: You can undo with <strong><span id="undo-shortcut-hint"></span></strong> and redo with <strong><span id="redo-shortcut-hint"></span></strong></li>
                </ol>
            </div>
        </div>

        <!-- Section 5: Reorder Elements -->
        <div class="paper-card section phcode-no-lp-edit-this" id="section-5" data-section="5">
            <div class="section-header phcode-no-lp-edit">
                <h2 class="section-title">Reorder elements by dragging</h2>
                <p class="section-subtitle">Drag any note to change the order.</p>
            </div>

            <div class="playground phcode-no-lp-edit-this">
                <div class="notes-container phcode-no-lp-edit-this">
                    <div class="sticky-note">Second Place</div>
                    <div class="sticky-note">First Place</div>
                    <div class="sticky-note">Third Place</div>
                </div>
            </div>

            <div class="success-message phcode-no-lp-edit" id="success-message-section5">
                <span class="success-icon">✓</span>
                <p id="success-message-text-section5">You can drag elements anywhere on the page to rearrange them.</p>
            </div>

            <div class="slide-controls phcode-no-lp-edit"></div>

            <div class="hint-content-inline phcode-no-lp-edit" data-section="5">
                <p style="margin-bottom: 12px; font-weight: 600; color: var(--text-primary);">To complete this task:</p>
                <ol style="margin: 0 0 0 20px; padding: 0;">
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Click and hold any note to start dragging</li>
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Move it up or down to change the order</li>
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Release to drop it in the new position</li>
                </ol>
            </div>
        </div>

        <!-- Section 6: Edit Links & Preview Mode (Final Slide) -->
        <div class="paper-card section phcode-no-lp-edit-this" id="section-6" data-section="6">
            <div class="section-header phcode-no-lp-edit">
                <h2 class="section-title">Edit links and preview your page</h2>
                <p class="section-subtitle">Select the <img src="images/link.svg" alt="Link" class="inline-icon"> icon in the toolbar to change where this link points.</p>
            </div>

            <div class="playground phcode-no-lp-edit-this">
                <div class="link-demo-container phcode-no-lp-edit-this">
                    <p class="link-demo-text"><a id="editable-link" href="https://docs.phcode.dev/app-links/live-preview-edit" class="demo-link" target="_blank">Edit Link</a></p>
                </div>
            </div>

            <div class="success-message phcode-no-lp-edit" id="success-message-section6">
                <span class="success-icon">✓</span>
                <p id="success-message-text-section6">You edited a link.</p>
            </div>

            <div class="mode-explanation phcode-no-lp-edit">
                <h3 class="mode-title">Edit mode vs Preview mode</h3>
                <div class="mode-cards" id="mode-cards-hover">
                    <div class="mode-card edit-mode-card">
                        <div class="mode-icon"><i class="fas fa-pencil"></i></div>
                        <div class="mode-info">
                            <strong>Edit mode</strong>
                            <p>Select and change things on the page</p>
                        </div>
                    </div>
                    <div class="mode-card preview-mode-card">
                        <div class="mode-icon"><i class="fas fa-pencil"></i></div>
                        <div class="mode-info">
                            <strong>Preview mode</strong>
                            <p>See your page as visitors will see it</p>
                        </div>
                    </div>
                </div>
                <p class="mode-hint">Press <strong>F8</strong> or click the <span class="play-button-hint"><i class="fa-solid fa-pencil"></i></span> button to switch modes.</p>
            </div>

            <div class="slide-controls phcode-no-lp-edit"></div>

            <div class="hint-content-inline phcode-no-lp-edit" data-section="6">
                <p style="margin-bottom: 12px; font-weight: 600; color: var(--text-primary);">Tips:</p>
                <ul style="margin: 0 0 0 20px; padding: 0; list-style-type: disc;">
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Click any link to select it for editing</li>
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Enable <strong>Open in new tab</strong> to open links on a separate page</li>
                    <li style="margin-bottom: 0; color: var(--text-primary);">Switch to Preview mode (<strong>F8</strong>) to test links and interactions</li>
                </ul>
            </div>
        </div>

        <!-- Section 7: Completion Slide -->
        <div class="paper-card section completion-slide phcode-no-lp-edit-this" id="section-7" data-section="7">
            <div class="completion-content phcode-no-lp-edit">
                <div class="completion-icon">
                    <img src="images/rocket.svg" alt="Rocket" class="completion-icon-svg svg-orange">
                </div>
                <h2 class="completion-title">You&#39;re ready to build!</h2>
                <p class="completion-body">You've learned how to edit with Live Preview.<br>Open a folder with your HTML files to start editing.</p>

                <p class="completion-helper">Need more help?</p>

                <div class="resource-links">
                    <a href="https://www.youtube.com/@phoenixcodeeditor3764" target="_blank" class="resource-link">
                        <i class="fab fa-youtube"></i>
                        <span>YouTube</span>
                    </a>
                    <a href="https://docs.phcode.dev/app-links/live-preview-edit" target="_blank" class="resource-link">
                        <i class="fas fa-book"></i>
                        <span>Documentation</span>
                    </a>
                </div>
            </div>

            <div class="slide-controls phcode-no-lp-edit"></div>
        </div>

        <!-- Section 8: Bonus - Check Layout with Measurements -->
        <div class="paper-card section bonus-slide phcode-no-lp-edit-this" id="section-8" data-section="8">
            <div class="section-header phcode-no-lp-edit">
                <span class="bonus-badge">One more thing</span>
                <h2 class="section-title">Check layout with measurements</h2>
                <p class="section-subtitle">Click <img src="images/verticalEllipsis.svg" alt="menu" class="inline-icon"> in the toolbar and select <strong>Show Measurements</strong> to see which box is misaligned.</p>
            </div>

            <div class="playground phcode-no-lp-edit-this">
                <div class="alignment-demo phcode-no-lp-edit-this">
                    <div class="align-box" id="align-box-1"><span class="phcode-no-lp-edit">Click <img src="images/verticalEllipsis.svg" alt="menu" class="inline-icon"></span></div>
                    <div class="align-box misaligned"><span>Box</span></div>
                    <div class="align-box"><span>Box</span></div>
                </div>
            </div>

            <div class="slide-controls phcode-no-lp-edit"></div>

            <div class="hint-content-inline phcode-no-lp-edit" data-section="8">
                <p style="margin-bottom: 12px; font-weight: 600; color: var(--text-primary);">Tips:</p>
                <ul style="margin: 0 0 0 20px; padding: 0; list-style-type: disc;">
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Click the <img src="images/verticalEllipsis.svg" alt="More options" class="inline-icon"> icon in the toolbar, or right-click to open the menu</li>
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Enable <strong>Show Measurements</strong> to see spacing and alignment guides</li>
                    <li style="margin-bottom: 8px; color: var(--text-primary);">Measurements help you spot alignment issues quickly</li>
                </ul>
            </div>
        </div>
    </main>

    <!-- Confetti Container for Celebrations -->
    <div class="confetti-container phcode-no-lp-edit" id="confetti-container"></div>

    <!-- Edit/Preview toggle arrow (shown on section 6 hover). Points at the
         pencil (edit-mode toggle) button in the live-preview toolbar. The
         former center arrow was removed — it used to point at the preview
         button when it was the dedicated play icon, but with the new
         single-pencil toggle there is only one target worth pointing at. -->
    <div class="arrow-pointer arrow-left" id="arrow-left">
        <img src="images/arrowUp.svg" alt="Arrow" class="arrow-svg svg-gold">
    </div>

    <script src="phoenix-pro.js"></script>
</body>
</html>
