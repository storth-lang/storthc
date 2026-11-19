;;; storth-mode.el -*- lexical-binding: t -*-

(require 'compile)
(require 'imenu)
(require 'thingatpt)

(defgroup storth nil
  "Major mode for editing Storth source files."
  :group 'languages
  :prefix "storth-")

(defface storth-keyword-face
  '((t :inherit font-lock-keyword-face))
  "Face for Storth keywords.")

(defface storth-type-face
  '((t :inherit font-lock-type-face))
  "Face for type names.")

(defface storth-constant-face
  '((t :inherit font-lock-constant-face))
  "Face for constants, literal keywords and labels.")

(defface storth-directive-face
  '((t :inherit font-lock-preprocessor-face))
  "Face for Storth directives such as #import.")

(defface storth-function-name-face
  '((t :inherit font-lock-function-name-face))
  "Face for function names at their definition.")

(defface storth-variable-name-face
  '((t :inherit font-lock-variable-name-face))
  "Face for names at their declaration.")

(defface storth-attribute-face
  '((t :inherit font-lock-preprocessor-face))
  "Face for '@attribute' names.")

(defface storth-builtin-face
  '((t :inherit font-lock-builtin-face))
  "Face for built-in functions such as the coroutine runtime.")

(defface storth-builtin-attribute-face
  '((t :inherit font-lock-builtin-face :weight bold))
  "Face for attributes handled by the compiler's default plugins.")

(defcustom storth-indent-offset 4
  "Number of spaces per indentation level in Storth."
  :type 'integer
  :safe #'integerp
  :group 'storth)

(defcustom storth-builtin-attributes '("format")
  "Attributes provided by the compiler's default plugins."
  :type '(repeat string)
  :group 'storth)

(defcustom storth-compiler "storthc"
  "Name or path of the Storth compiler executable."
  :type 'string
  :group 'storth)

(defcustom storth-highlight-capitalized-types t
  "When non-nil, highlight Capitalized identifiers as type names."
  :type 'boolean
  :group 'storth)

(defconst storth-keywords
  '("fn" "return" "if" "else" "then" "while" "for"
    "defer" "break" "continue" "static" "union" "impl"
    "struct" "enum" "pub" "extern" "using" "trait"
    "enum_flag" "type_info" "noreturn" "cast"
    "sizeof" "align_of" "type_of" "kind" "cstr" "fields" "str_from_raw"
    "case" "goto" "label" "default" "where" "self" "xx" "not"
    "and" "or" "inline")
  "Storth keywords.")

(defconst storth-builtins
  '("co_resume" "co_done" "co_free")
  "Storth built-in functions.")

(defconst storth-constants
  '("true" "false" "null")
  "Storth literal keywords.")

(defconst storth-directives
  '("#import" "#load" "#plugin" "#fields"
    "#comptime" "#if" "#else" "#comp_error"
    "#asm" "#pad" "#pack" "#union" "#expand" "#body")
  "Storth directives.")

(defconst storth-types
  '("i8" "i16" "i32" "i64"
    "u8" "u16" "u32" "u64"
    "f32" "f64" "f128"
    "bool" "char" "string" "void" "any"
    "va_list" "Location" "Self" "Co" "CoTask")
  "Storth built-in type names.")

(defconst storth--ident-re "[A-Za-z_][A-Za-z0-9_]*"
  "Regexp matching a Storth identifier.")

(defvar storth-mode-syntax-table
  (let ((st (make-syntax-table)))
    (modify-syntax-entry ?/ ". 124b" st)
    (modify-syntax-entry ?* ". 23" st)
    (modify-syntax-entry ?\n "> b" st)
    (modify-syntax-entry ?\" "\"" st)
    (modify-syntax-entry ?' "\"" st)
    (modify-syntax-entry ?\\ "\\" st)
    (modify-syntax-entry ?_ "_" st)
    (modify-syntax-entry ?# "'" st)
    (modify-syntax-entry ?@ "'" st)
    (modify-syntax-entry ?$ "'" st)
    (dolist (c '(?+ ?- ?= ?< ?> ?& ?| ?% ?! ?^ ?~ ?: ?. ?, ?\;))
      (modify-syntax-entry c "." st))
    st)
  "Syntax table for `storth-mode'.")

(defun storth--in-code-p (pos)
  "Non-nil if POS is outside strings and comments."
  (not (nth 8 (save-excursion (syntax-ppss pos)))))

(defun storth--match-declared-name (limit)
  "Find the next name being declared before LIMIT.
That is an identifier followed by a colon, as in a declaration, a
parameter, a constant or one name of a multi-bind."
  (let (found)
    (while (and (not found)
                (re-search-forward (concat "\\_<" storth--ident-re "\\_>") limit t))
      (let ((beg (match-beginning 0))
            (end (match-end 0))
            (name (match-string-no-properties 0)))
        (when (and (storth--in-code-p beg)
                   (not (member name storth-keywords))
                   (not (member name storth-constants))
                   (not (member name storth-types))
                   (not (member name storth-builtins))
                   (not (memq (char-before beg) '(?. ?$ ?# ?@)))
                   (save-excursion
                     (goto-char end)
                     (looking-at (concat "\\(?:[ \t]*,[ \t]*" storth--ident-re "\\)*[ \t]*:")))
                   (save-excursion
                     (goto-char beg)
                     (skip-chars-backward " \t")
                     (not (looking-back "\\_<\\(?:case\\|default\\)" (line-beginning-position)))))
          (set-match-data (list beg end))
          (setq found t))))
    found))

(defun storth--match-capitalized-type (limit)
  "Find the next Capitalized identifier in code before LIMIT."
  (when storth-highlight-capitalized-types
    (let ((case-fold-search nil)
          found)
      (while (and (not found)
                  (re-search-forward "\\_<[A-Z][A-Za-z0-9_]*[a-z][A-Za-z0-9_]*\\_>" limit t))
        (when (and (storth--in-code-p (match-beginning 0))
                   (not (memq (char-before (match-beginning 0)) '(?. ?$ ?# ?@))))
          (setq found t)))
      found)))

(defconst storth-font-lock-keywords
  (let ((kw-re (regexp-opt storth-keywords 'symbols))
        (type-re (regexp-opt storth-types 'symbols))
        (const-re (regexp-opt storth-constants 'symbols))
        (builtin-re (regexp-opt storth-builtins 'symbols))
        (id storth--ident-re))
    `((,(concat "@\\(" (regexp-opt storth-builtin-attributes) "\\)\\_>")
       (0 'storth-builtin-attribute-face))
      (,(concat "@" id) . 'storth-attribute-face)
      (,(concat "#" id) . 'storth-directive-face)
      ("\\_<\\(co\\)[ \t]+fn\\_>"
       (1 'storth-keyword-face))
      (,(concat "\\_<fn[ \t]+\\(" id "\\)")
       (1 'storth-function-name-face))
      ("\\(?:^\\|[^.[:alnum:]_]\\)\\(yield\\)[ \t]*("
       (1 'storth-keyword-face))
      (,(concat "\\_<\\(?:struct\\|enum\\|enum_flag\\|tag_union\\|trait\\)[ \t]+\\(" id "\\)")
       (1 'storth-type-face))
      (,(concat "\\_<\\(?:goto\\|label\\)[ \t]+\\(" id "\\)")
       (1 'storth-constant-face))
      (,kw-re . 'storth-keyword-face)
      (,const-re . 'storth-constant-face)
      (,builtin-re . 'storth-builtin-face)
      (,type-re . 'storth-type-face)
      (,(concat "\\$" id) . 'storth-type-face)
      (storth--match-capitalized-type . 'storth-type-face)
      (,(concat "\\_<[A-Z][A-Z0-9_]+\\_>") . 'storth-constant-face)
      (storth--match-declared-name . 'storth-variable-name-face)))
  "Font-lock keywords for `storth-mode'.")

(defconst storth--continuation-re
  "\\(?:[-+*/%=<>|&^]\\|\\_<then\\)"
  "A line whose code ends in this continues onto the next line.")

(defun storth--code-end ()
  "Return the position just after the last code character on the current line."
  (save-excursion
    (end-of-line)
    (let ((ppss (syntax-ppss)))
      (when (nth 4 ppss)
        (goto-char (nth 8 ppss))))
    (skip-chars-backward " \t")
    (max (point) (line-beginning-position))))

(defun storth--line-continues-p ()
  "Non-nil if the current line's statement carries on to the next line."
  (save-excursion
    (back-to-indentation)
    (unless (looking-at "@\\|#\\|$\\|//")
      (let ((beg (line-beginning-position))
            (end (storth--code-end)))
        (and (> end beg)
             (not (nth 4 (save-excursion (syntax-ppss (1- end)))))
             (not (nth 3 (save-excursion (syntax-ppss end))))
             (save-excursion
               (goto-char end)
               (looking-back storth--continuation-re beg)))))))

(defun storth--prev-code-line ()
  "Move to the previous line holding code. Return non-nil if one was found."
  (let (found)
    (while (and (not found) (= 0 (forward-line -1)))
      (back-to-indentation)
      (unless (or (looking-at "$")
                  (looking-at "//")
                  (nth 4 (syntax-ppss)))
        (setq found t)))
    found))

(defun storth--escape-closed-parens ()
  "Move to the opener of a paren group open at line start but closed before point."
  (let ((moved t))
    (while moved
      (setq moved nil)
      (let ((enclosing (nth 9 (syntax-ppss (point))))
            (opens (nth 9 (save-excursion (syntax-ppss (line-beginning-position)))))
            (target nil))
        (dolist (p opens)
          (when (and (not target)
                     (memq (char-after p) '(?\( ?\[))
                     (not (memq p enclosing)))
            (setq target p)))
        (when target
          (goto-char target)
          (setq moved t))))))

(defun storth--stmt-indent ()
  "Indentation of the statement the current line belongs to."
  (save-excursion
    (storth--escape-closed-parens)
    (beginning-of-line)
    (let ((done nil))
      (while (not done)
        (let ((here (point)))
          (if (and (storth--prev-code-line) (storth--line-continues-p))
              (beginning-of-line)
            (goto-char here)
            (setq done t)))))
    (current-indentation)))

(defun storth--calculate-indent ()
  "Return the column the current line should be indented to, or nil to leave it."
  (save-excursion
    (back-to-indentation)
    (let* ((ppss (syntax-ppss))
           (open (nth 1 ppss)))
      (cond
       ((nth 3 ppss) nil)
       ((nth 4 ppss)
        (let ((star (looking-at "\\*")))
          (save-excursion
            (goto-char (nth 8 ppss))
            (when (looking-at "/\\*")
              (+ (current-column) (if star 1 3))))))
       (t
        (let* ((closer (looking-at "[]})]"))
               (in-brace (or (null open) (eq (char-after open) ?{)))
               (base
                (cond
                 ((null open) 0)
                 ((eq (char-after open) ?{)
                  (save-excursion
                    (goto-char open)
                    (+ (storth--stmt-indent) (if closer 0 storth-indent-offset))))
                 (t
                  (save-excursion
                    (goto-char open)
                    (cond
                     (closer (storth--stmt-indent))
                     ((progn (forward-char 1)
                             (skip-chars-forward " \t")
                             (not (or (eolp) (looking-at "//"))))
                      (current-column))
                     (t (+ (storth--stmt-indent) storth-indent-offset))))))))
          (if (and in-brace
                   (not closer)
                   (not (looking-at "{\\|else\\_>\\|case\\_>\\|default\\_>\\|#else\\_>\\|#case\\_>"))
                   (save-excursion
                     (and (storth--prev-code-line)
                          (or (null open) (> (point) open))
                          (storth--line-continues-p))))
              (+ base storth-indent-offset)
            base)))))))

(defun storth-indent-line ()
  "Indent the current line as Storth code."
  (interactive)
  (let ((indent (storth--calculate-indent)))
    (when indent
      (save-excursion
        (back-to-indentation)
        (unless (= (current-column) indent)
          (delete-horizontal-space)
          (indent-to indent)))
      (when (< (current-column) indent)
        (back-to-indentation)))))

(defconst storth--defun-start-re
  (concat "^\\(?:pub[ \t]+\\)?"
          "\\(?:inline[ \t]+\\)?"
          "\\(?:\\(?:where\\|fn\\|co[ \t]+fn\\|struct\\|enum\\|enum_flag\\|tag_union\\|trait\\|extern\\)\\_>"
          "\\|#template\\_>"
          "\\|" storth--ident-re "[ \t]*:\\)")
  "Regexp matching the first line of a top-level declaration.")

(defun storth--after-where-p ()
  "Non-nil if the line before point's line is a where clause."
  (save-excursion
    (and (= 0 (forward-line -1))
         (looking-at "^\\(?:pub[ \t]+\\)?where\\_>"))))

(defun storth--defun-search (backward)
  "Move to the next top-level declaration start, searching BACKWARD if non-nil."
  (let (found)
    (while (and (not found)
                (if backward
                    (re-search-backward storth--defun-start-re nil t)
                  (re-search-forward storth--defun-start-re nil t)))
      (goto-char (match-beginning 0))
      (if (and (storth--in-code-p (point))
               (or backward (not (storth--after-where-p))))
          (setq found t)
        (unless backward
          (end-of-line))))
    (when found
      (while (storth--after-where-p)
        (forward-line -1)))
    found))

(defun storth-beginning-of-defun (&optional arg)
  "Move to the start of the ARGth top-level declaration before point."
  (setq arg (or arg 1))
  (let ((found t))
    (if (> arg 0)
        (dotimes (_ arg)
          (when found
            (setq found (storth--defun-search t))))
      (dotimes (_ (- arg))
        (when found
          (end-of-line)
          (setq found (storth--defun-search nil)))))
    found))

(defun storth-end-of-defun ()
  "Move from the start of a top-level declaration to just after its end."
  (let ((done nil))
    (while (and (not done) (re-search-forward "[{;]" nil t))
      (let ((ppss (save-excursion (syntax-ppss (match-beginning 0)))))
        (when (and (not (nth 8 ppss)) (= 0 (nth 0 ppss)))
          (if (eq (char-before) ?\;)
              (setq done t)
            (backward-char)
            (condition-case nil
                (forward-sexp)
              (scan-error (goto-char (point-max))))
            (skip-chars-forward " \t")
            (when (eq (char-after) ?\;)
              (forward-char))
            (setq done t)))))
    (unless done
      (goto-char (point-max)))))

(defconst storth-imenu-generic-expression
  (let ((id storth--ident-re))
    `(("Functions"
       ,(concat "^[ \t]*\\(?:#template[ \t]+\\)?\\(?:pub[ \t]+\\)?\\(?:inline[ \t]+\\)?fn[ \t]+\\(" id "\\)") 1)
      ("Coroutines"
       ,(concat "^[ \t]*\\(?:pub[ \t]+\\)?co[ \t]+fn[ \t]+\\(" id "\\)") 1)
      ("Structs"
       ,(concat "^\\(?:pub[ \t]+\\)?struct[ \t]+\\(" id "\\)") 1)
      ("Enums"
       ,(concat "^\\(?:pub[ \t]+\\)?\\(?:enum\\|enum_flag\\)[ \t]+\\(" id "\\)") 1)
      ("Tag unions"
       ,(concat "^\\(?:pub[ \t]+\\)?tag_union[ \t]+\\(" id "\\)") 1)
      ("Traits"
       ,(concat "^\\(?:pub[ \t]+\\)?trait[ \t]+\\(" id "\\)") 1)
      ("Impls"
       ,(concat "^\\(?:pub[ \t]+\\)?\\(" id "\\)[ \t]*:=[ \t]*" id "[ \t]*(.*)[ \t]*{") 1)
      ("Globals"
       ,(concat "^\\(?:pub[ \t]+\\)?\\(" id "\\)[ \t]*:[^({\n]*;") 1)))
  "Imenu expressions for `storth-mode'.")

(defun storth-completion-at-point ()
  "Complete Storth keywords, types, constants and directives at point."
  (let ((bounds (bounds-of-thing-at-point 'symbol)))
    (cond
     ((and bounds (eq (char-before (car bounds)) ?#))
      (list (car bounds) (cdr bounds)
            (mapcar (lambda (d) (substring d 1)) storth-directives)
            :exclusive 'no))
     ((eq (char-before) ?#)
      (list (point) (point)
            (mapcar (lambda (d) (substring d 1)) storth-directives)
            :exclusive 'no))
     (bounds
      (list (car bounds) (cdr bounds)
            (append storth-keywords storth-types storth-constants storth-builtins '("co" "yield"))
            :exclusive 'no)))))

(defun storth--run-command ()
  "Return the command that runs the current file."
  (format "%s run %s" storth-compiler
          (shell-quote-argument
           (if buffer-file-name
               (file-relative-name buffer-file-name)
             (buffer-name)))))

(defun storth-run ()
  "Save the buffer, then compile and run it with `storth-compiler'."
  (interactive)
  (when (and buffer-file-name (buffer-modified-p))
    (save-buffer))
  (compile (storth--run-command)))

(defvar storth-mode-map
  (let ((map (make-sparse-keymap)))
    (define-key map (kbd "C-c C-c") #'storth-run)
    (define-key map (kbd "C-c C-k") #'compile)
    map)
  "Keymap for `storth-mode'.")

(defconst storth-compilation-error-regexp
  '(storth
    "^\\([^ \t\n][^\t\n]*\\.st\\):\\([0-9]+\\):\\([0-9]+\\): error:"
    1 2 3 2))

(defconst storth-compilation-warning-regexp
  '(storth-warning
    "^\\([^ \t\n][^\t\n]*\\.st\\):\\([0-9]+\\):\\([0-9]+\\): warning:"
    1 2 3 1))

(defconst storth-compilation-note-regexp
  '(storth-note
    "^\\([^ \t\n][^\t\n]*\\.st\\):\\([0-9]+\\):\\([0-9]+\\): note:"
    1 2 3 0))

(with-eval-after-load 'compile
  (dolist (re (list storth-compilation-error-regexp
                    storth-compilation-warning-regexp
                    storth-compilation-note-regexp))
    (add-to-list 'compilation-error-regexp-alist-alist re)
    (add-to-list 'compilation-error-regexp-alist (car re))))

(with-eval-after-load 'hideshow
  (add-to-list 'hs-special-modes-alist
               '(storth-mode "{" "}" "/[*/]" nil nil)))

(define-derived-mode storth-mode prog-mode "Storth"
  "Major mode for editing Storth source files.

\\{storth-mode-map}"
  :syntax-table storth-mode-syntax-table

  (setq-local comment-start "// ")
  (setq-local comment-end "")
  (setq-local comment-start-skip "\\(?://+\\|/\\*+\\)[ \t]*")
  (setq-local comment-end-skip "[ \t]*\\(?:\n\\|\\*+/\\)")
  (setq-local comment-multi-line t)
  (setq-local adaptive-fill-regexp "[ \t]*\\(?://+\\|\\*+\\)?[ \t]*")
  (setq-local parse-sexp-ignore-comments t)

  (setq-local font-lock-defaults '(storth-font-lock-keywords nil nil nil nil))

  (setq-local indent-line-function #'storth-indent-line)
  (setq-local electric-indent-chars (append '(?} ?\) ?\] ?\;) electric-indent-chars))
  (setq-local tab-width storth-indent-offset)
  (setq-local indent-tabs-mode nil)

  (setq-local beginning-of-defun-function #'storth-beginning-of-defun)
  (setq-local end-of-defun-function #'storth-end-of-defun)
  (setq-local outline-regexp "\\(?:pub[ \t]+\\)?\\(?:inline[ \t]+\\)?\\(?:fn\\|co[ \t]+fn\\|struct\\|enum\\|enum_flag\\|tag_union\\|trait\\)\\_>")

  (setq-local imenu-generic-expression storth-imenu-generic-expression)
  (setq-local imenu-case-fold-search nil)
  (imenu-add-to-menubar "Storth")

  (add-hook 'completion-at-point-functions #'storth-completion-at-point nil t)

  (setq-local compile-command (storth--run-command)))

(add-to-list 'auto-mode-alist '("\\.st\\'" . storth-mode))

(provide 'storth-mode)
